// Copyright 2021-present MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cast_core/actors/MoveRandomChunkToRandomShard.hpp>

#include <memory>

#include <yaml-cpp/yaml.h>

#include <bsoncxx/json.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>

#include <boost/log/trivial.hpp>
#include <boost/throw_exception.hpp>

#include <gennylib/Cast.hpp>
#include <gennylib/MongoException.hpp>
#include <gennylib/context.hpp>

#include <value_generators/DocumentGenerator.hpp>

#include <bsoncxx/builder/stream/document.hpp>

namespace genny::actor {

struct MoveRandomChunkToRandomShard::PhaseConfig {
    std::string collectionNamespace;
    bool forceJumbo;

    PhaseConfig(PhaseContext& context, ActorId id)
        : collectionNamespace{context["Namespace"].to<std::string>()},
          forceJumbo{context["ForceJumbo"].maybe<bool>().value_or(false)} {}
};

// Increase default wait timeout for range deletions on recipients shards when doing a migration.
void setRecipientRangeDeletionWaitTimeout(const mongocxx::database& configDatabase) {
    bsoncxx::document::value setParameterDocument = bsoncxx::builder::stream::document()
        << "setParameter" << 1 << "receiveChunkWaitForRangeDeleterTimeoutMS" << 3600000
        << bsoncxx::builder::stream::finalize;
    bsoncxx::document::value emptyFilter = bsoncxx::builder::stream::document()
        << bsoncxx::builder::stream::finalize;
    auto shardsCursor = configDatabase["shards"].find(emptyFilter.view());
    for (auto&& shardDoc : shardsCursor) {
        std::string fullHost = shardDoc["host"].get_utf8().value.to_string();
        std::stringstream mongoURI;
        mongoURI << "mongodb://" << fullHost.substr(fullHost.find("/") + 1);
        mongocxx::client shardClient(mongocxx::uri(mongoURI.str()));
        shardClient.database("admin").run_command(setParameterDocument.view());
    }
}

bsoncxx::document::value getServerStatus(mongocxx::pool::entry& client,
                                         const std::string& shardId) {
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    auto&& configDatabase = client->database("config");
    auto shardOpt = configDatabase["shards"].find_one(make_document(kvp("_id", shardId)));
    assert(shardOpt.is_initialized());
    const std::string fullHost = shardOpt.get().view()["host"].get_utf8().value.to_string();

    std::stringstream mongoURI;
    mongoURI << "mongodb://" << fullHost.substr(fullHost.find("/") + 1);

    mongocxx::client shardClient(mongocxx::uri(mongoURI.str()));

    auto serverStatus =
        shardClient.database("admin").run_command(make_document(kvp("serverStatus", 1)));
    return serverStatus;
}

void MoveRandomChunkToRandomShard::run() {
    uint64_t iteration = 0;
    setRecipientRangeDeletionWaitTimeout(_client->database("config"));
    for (auto&& config : _loop) {
        for (const auto&& _ : config) {
            iteration++;

            auto&& configDatabase = _client->database("config");
            // Get the collection uuid.
            bsoncxx::document::value collectionFilter = bsoncxx::builder::stream::document()
                << "_id" << config->collectionNamespace << bsoncxx::builder::stream::finalize;
            auto collectionDocOpt = configDatabase["collections"].find_one(collectionFilter.view());
            // There must be a collection with the provided namespace.
            assert(collectionDocOpt.is_initialized());
            auto uuid = collectionDocOpt.get().view()["uuid"].get_binary();

            // Select a random chunk.
            bsoncxx::document::value uuidDoc = bsoncxx::builder::stream::document()
                << "uuid" << uuid << bsoncxx::builder::stream::finalize;
            // This is for backward compatibility, before 5.0 chunks were indexed by namespace.
            bsoncxx::document::value nsDoc = bsoncxx::builder::stream::document()
                << "ns" << config->collectionNamespace << bsoncxx::builder::stream::finalize;
            bsoncxx::document::value chunksFilter = bsoncxx::builder::stream::document()
                << "$or" << bsoncxx::builder::stream::open_array << uuidDoc.view() << nsDoc.view()
                << bsoncxx::builder::stream::close_array << bsoncxx::builder::stream::finalize;
            auto numChunks = configDatabase["chunks"].count_documents(chunksFilter.view());
            // The collection must have been sharded and must have at least one chunk;
            boost::random::uniform_int_distribution<int64_t> chunkUniformDistribution(
                0, (int)numChunks - 1);
            mongocxx::options::find chunkFindOptions;
            auto chunkSort = bsoncxx::builder::stream::document()
                << "lastmod" << 1 << bsoncxx::builder::stream::finalize;
            chunkFindOptions.sort(chunkSort.view());
            chunkFindOptions.skip(chunkUniformDistribution(_rng));
            chunkFindOptions.limit(1);
            auto chunkProjection = bsoncxx::builder::stream::document()
                << "history" << false << bsoncxx::builder::stream::finalize;
            chunkFindOptions.projection(chunkProjection.view());
            auto chunkCursor = configDatabase["chunks"].find(chunksFilter.view(), chunkFindOptions);
            assert(chunkCursor.begin() != chunkCursor.end());
            bsoncxx::v_noabi::document::view chunk = *chunkCursor.begin();

            // Find a destination shard different to the source.
            bsoncxx::document::value notEqualDoc = bsoncxx::builder::stream::document()
                << "$ne" << chunk["shard"].get_utf8() << bsoncxx::builder::stream::finalize;
            bsoncxx::document::value shardFilter = bsoncxx::builder::stream::document()
                << "_id" << notEqualDoc.view() << bsoncxx::builder::stream::finalize;
            auto numShards = configDatabase["shards"].count_documents(shardFilter.view());
            boost::random::uniform_int_distribution<int64_t> shardUniformDistribution(
                0, (int)numShards - 1);
            mongocxx::options::find shardFindOptions;
            auto shardSort = bsoncxx::builder::stream::document()
                << "_id" << 1 << bsoncxx::builder::stream::finalize;
            shardFindOptions.sort(shardSort.view());
            shardFindOptions.skip(shardUniformDistribution(_rng));
            shardFindOptions.limit(1);
            auto shardCursor = configDatabase["shards"].find(shardFilter.view(), shardFindOptions);
            // There must be at least 1 shard, which will be the destination.
            assert(shardCursor.begin() != shardCursor.end());
            bsoncxx::v_noabi::document::view shard = *shardCursor.begin();

            bsoncxx::document::value moveChunkCmd = bsoncxx::builder::stream::document{}
                << "moveChunk" << config->collectionNamespace << "bounds"
                << bsoncxx::builder::stream::open_array << chunk["min"].get_value()
                << chunk["max"].get_value() << bsoncxx::builder::stream::close_array << "to"
                << shard["_id"].get_utf8() << "forceJumbo" << config->forceJumbo << "maxTimeMS"
                << 3600000 << "_waitForDelete" << true << bsoncxx::builder::stream::finalize;
            // This is only for better readability when logging.
            auto bounds = bsoncxx::builder::stream::document()
                << "bounds" << bsoncxx::builder::stream::open_array << chunk["min"].get_value()
                << chunk["max"].get_value() << bsoncxx::builder::stream::close_array
                << bsoncxx::builder::stream::finalize;
            BOOST_LOG_TRIVIAL(info)
                << "MoveChunkToRandomShardActor (iteration " << iteration << ") "
                << "moving chunk " << bsoncxx::to_json(bounds.view())
                << " from: " << chunk["shard"].get_utf8().value.to_string()
                << " to: " << shard["_id"].get_utf8().value.to_string();

            const std::string donorShardId = chunk["shard"].get_utf8().value.to_string();
            const std::string recipientShardId = shard["_id"].get_utf8().value.to_string();

            const auto beforeMigrationDonorServerStatus = getServerStatus(_client, donorShardId);
            auto beforeMigrationDonorShardingStatistics =
                beforeMigrationDonorServerStatus.view()["shardingStatistics"];

            const auto beforeMigrationRecipientServerStatus =
                getServerStatus(_client, recipientShardId);
            const auto beforeMigrationRecipientShardingStatistics =
                beforeMigrationRecipientServerStatus.view()["shardingStatistics"];

            auto totalOpCtx = _totalMoveChunk.start();
            try {
                _client->database("admin").run_command(moveChunkCmd.view());
                totalOpCtx.success();

                // Get move chunk statistics.
                const auto donorServerStatus = getServerStatus(_client, donorShardId);
                const auto donorShardingStatistics = donorServerStatus.view()["shardingStatistics"];

                const auto recipientServerStatus = getServerStatus(_client, recipientShardId);
                const auto recipientShardingStatistics =
                    recipientServerStatus.view()["shardingStatistics"];

                auto now = metrics::clock::now();
                _totalDonorChunkCloneTimeMillis.report(
                    now,
                    std::chrono::microseconds(
                        donorShardingStatistics["totalDonorChunkCloneTimeMillis"].get_int64() -
                        beforeMigrationDonorShardingStatistics["totalDonorChunkCloneTimeMillis"]
                            .get_int64()),
                    metrics::OutcomeType::kSuccess);
                _totalCriticalSectionCommitTimeMillis.report(
                    now,
                    std::chrono::microseconds(
                        donorShardingStatistics["totalCriticalSectionCommitTimeMillis"]
                            .get_int64() -
                        beforeMigrationDonorShardingStatistics
                            ["totalCriticalSectionCommitTimeMillis"]
                                .get_int64()),
                    metrics::OutcomeType::kSuccess);
                _totalCriticalSectionTimeMillis.report(
                    now,
                    std::chrono::microseconds(
                        donorShardingStatistics["totalCriticalSectionTimeMillis"].get_int64() -
                        beforeMigrationDonorShardingStatistics["totalCriticalSectionTimeMillis"]
                            .get_int64()),
                    metrics::OutcomeType::kSuccess);

                // Require SERVER-60984
                assert(recipientShardingStatistics["totalRecipientCriticalSectionTimeMillis"]);

                _totalRecipientCriticalSectionTimeMillis.report(
                    now,
                    std::chrono::microseconds(
                        recipientShardingStatistics["totalRecipientCriticalSectionTimeMillis"]
                            .get_int64() -
                        beforeMigrationRecipientShardingStatistics
                            ["totalRecipientCriticalSectionTimeMillis"]
                                .get_int64()),
                    metrics::OutcomeType::kSuccess);
            } catch (mongocxx::operation_exception& e) {
                BOOST_LOG_TRIVIAL(info) << "A move chunk failed: " << e.what();
                totalOpCtx.failure();
                auto now = metrics::clock::now();
                _totalDonorChunkCloneTimeMillis.report(
                    now, std::chrono::microseconds(0), metrics::OutcomeType::kFailure);
                _totalCriticalSectionCommitTimeMillis.report(
                    now, std::chrono::microseconds(0), metrics::OutcomeType::kFailure);
                _totalCriticalSectionTimeMillis.report(
                    now, std::chrono::microseconds(0), metrics::OutcomeType::kFailure);
                _totalCriticalSectionCSWriteTimeMillis.report(
                    now, std::chrono::microseconds(0), metrics::OutcomeType::kFailure);
                _totalCriticalSectionRefreshTimeMillis.report(
                    now, std::chrono::microseconds(0), metrics::OutcomeType::kFailure);
                _totalRecipientCriticalSectionTimeMillis.report(
                    now, std::chrono::microseconds(0), metrics::OutcomeType::kFailure);
            }
        }
    }
}

MoveRandomChunkToRandomShard::MoveRandomChunkToRandomShard(genny::ActorContext& context)
    : Actor{context},
      _rng{context.workload().getRNGForThread(MoveRandomChunkToRandomShard::id())},
      _client{context.client()},
      _loop{context, MoveRandomChunkToRandomShard::id()},
      _totalMoveChunk{context.operation("totalMoveChunk", MoveRandomChunkToRandomShard::id())},
      _totalDonorChunkCloneTimeMillis{
          context.operation("totalDonorChunkCloneTimeMillis", MoveRandomChunkToRandomShard::id())},
      _totalCriticalSectionCommitTimeMillis{context.operation(
          "totalCriticalSectionCommitTimeMillis", MoveRandomChunkToRandomShard::id())},
      _totalCriticalSectionTimeMillis{
          context.operation("totalCriticalSectionTimeMillis", MoveRandomChunkToRandomShard::id())},
      _totalCriticalSectionCSWriteTimeMillis{context.operation(
          "totalCriticalSectionCSWriteTimeMillis", MoveRandomChunkToRandomShard::id())},
      _totalCriticalSectionRefreshTimeMillis{context.operation(
          "totalCriticalSectionRefreshTimeMillis", MoveRandomChunkToRandomShard::id())},
      _totalRecipientCriticalSectionTimeMillis{context.operation(
          "totalRecipientCriticalSectionTimeMillis", MoveRandomChunkToRandomShard::id())} {}

namespace {
//
// This tells the "global" cast about our actor using the defaultName() method
// in the header file.
//
auto registerMoveRandomChunkToRandomShard = Cast::registerDefault<MoveRandomChunkToRandomShard>();
}  // namespace
}  // namespace genny::actor
