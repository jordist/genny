// Copyright 2019-present MongoDB Inc.
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

#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

#include <metrics/MetricsReporter.hpp>
#include <metrics/metrics.hpp>

#include <testlib/ActorHelper.hpp>
#include <testlib/helpers.hpp>

namespace genny::metrics {
namespace {

using namespace std::literals::chrono_literals;

class RegistryClockSourceStub {
private:
    using clock_type = std::chrono::steady_clock;

public:
    using duration = clock_type::duration;
    using time_point = std::chrono::time_point<clock_type>;

    static void advance(v1::Period<clock_type> inc = 1ns) {
        _now += inc;
    }

    static void reset() {
        _now = {};
    }

    static time_point now() {
        return _now;
    }

private:
    static time_point _now;
};

RegistryClockSourceStub::time_point RegistryClockSourceStub::_now;

struct ReporterClockSourceStub {
    using time_point = std::chrono::time_point<std::chrono::system_clock>;

    static time_point now() {
        return time_point{42ms};
    }
};

void assertDurationsEqual(RegistryClockSourceStub::duration dur1,
                          RegistryClockSourceStub::duration dur2) {
    // We compare std:chrono::time_points by converting through Period in order to take advantage of
    // its operator<<(std::ostream&) in case the check fails.
    REQUIRE(v1::Period<RegistryClockSourceStub>{dur1} == v1::Period<RegistryClockSourceStub>{dur2});
}

TEST_CASE("metrics::OperationContext interface") {
    RegistryClockSourceStub::reset();

    auto op = v1::OperationImpl<RegistryClockSourceStub>{"Actor", "Op"};

    RegistryClockSourceStub::advance(5ns);
    auto ctx = std::make_optional<v1::OperationContextT<RegistryClockSourceStub>>(&op);

    ctx->addDocuments(200);
    ctx->addBytes(3000);
    ctx->addErrors(4);
    RegistryClockSourceStub::advance(67ns);

    REQUIRE(op.getEvents().size() == 0);

    auto expected = v1::OperationEvent<RegistryClockSourceStub>{};
    expected.iters = 1;
    expected.ops = 200;
    expected.size = 3000;
    expected.errors = 4;

    SECTION("success() reports the operation") {
        expected.duration = 67ns;
        ctx->success();
        REQUIRE(op.getEvents().size() == 1);

        ctx.reset();

        expected.outcome = v1::OperationEvent<RegistryClockSourceStub>::OutcomeType::kSuccess;
        assertDurationsEqual(op.getEvents()[0].first.time_since_epoch(), 72ns);
        REQUIRE(op.getEvents()[0].second == expected);
    }

    SECTION("failure() reports the operation") {
        expected.duration = 67ns;
        ctx->failure();
        REQUIRE(op.getEvents().size() == 1);

        ctx.reset();

        expected.outcome = v1::OperationEvent<RegistryClockSourceStub>::OutcomeType::kFailure;
        assertDurationsEqual(op.getEvents()[0].first.time_since_epoch(), 72ns);
        REQUIRE(op.getEvents()[0].second == expected);
    }

    SECTION("discard() doesn't report the operation") {
        ctx.reset();
        REQUIRE(op.getEvents().size() == 0);
    }

    SECTION("add*() methods can be called multiple times") {
        ctx->addIterations(8);
        ctx->addIterations(9);
        ctx->addDocuments(200);
        ctx->addBytes(3000);
        ctx->addErrors(4);
        RegistryClockSourceStub::advance(67ns);

        REQUIRE(op.getEvents().size() == 0);

        expected.iters = 17;
        expected.ops += 200;
        expected.size += 3000;
        expected.errors += 4;

        expected.duration = 134ns;
        ctx->success();
        REQUIRE(op.getEvents().size() == 1);

        ctx.reset();

        expected.outcome = v1::OperationEvent<RegistryClockSourceStub>::OutcomeType::kSuccess;
        assertDurationsEqual(op.getEvents()[0].first.time_since_epoch(), 139ns);
        REQUIRE(op.getEvents()[0].second == expected);
    }
}

TEST_CASE("metrics output format") {
    RegistryClockSourceStub::reset();
    auto metrics = v1::RegistryT<RegistryClockSourceStub>{};
    auto reporter = genny::metrics::v1::ReporterT{metrics};

    //           +---------------------+----------------+
    // Thread 1: |        Insert       |     Remove     |
    //           +---------------------+----------------+
    //                +------------------+  +--------+
    // Thread 2:      |      Insert      |  | Remove |
    //                +------------------+  +--------+
    //                   +-----------+
    // Thread 3:         | Greetings |
    //                   +-----------+

    auto insert1 = metrics.operation("InsertRemove", "Insert", 1u);
    auto insert2 = metrics.operation("InsertRemove", "Insert", 2u);
    auto remove1 = metrics.operation("InsertRemove", "Remove", 1u);
    auto remove2 = metrics.operation("InsertRemove", "Remove", 2u);
    auto greetings3 = metrics.operation("HelloWorld", "Greetings", 3u);

    RegistryClockSourceStub::advance(5ns);
    auto insert1Ctx = insert1.start();

    RegistryClockSourceStub::advance(5ns);
    auto insert2Ctx = insert2.start();

    RegistryClockSourceStub::advance(3ns);
    auto greetings3Ctx = greetings3.start();

    RegistryClockSourceStub::advance(13ns);
    greetings3Ctx.addIterations(2);
    greetings3Ctx.success();

    RegistryClockSourceStub::advance(2ns);
    insert1Ctx.addDocuments(9);
    insert1Ctx.addBytes(300);
    insert1Ctx.success();
    auto remove1Ctx = remove1.start();

    RegistryClockSourceStub::advance(2ns);
    insert2Ctx.addDocuments(8);
    insert2Ctx.addBytes(200);
    insert2Ctx.success();

    RegistryClockSourceStub::advance(2ns);
    auto remove2Ctx = remove2.start();

    RegistryClockSourceStub::advance(10ns);
    remove2Ctx.addDocuments(7);
    remove2Ctx.addBytes(30);
    remove2Ctx.success();

    RegistryClockSourceStub::advance(3ns);
    remove1Ctx.addDocuments(6);
    remove1Ctx.addBytes(40);
    remove1Ctx.success();

    SECTION("csv reporting") {
        auto expected =
            "Clocks\n"
            "SystemTime,42000000\n"
            "MetricsTime,45\n"
            "\n"
            "Counters\n"
            "26,HelloWorld.id-3.Greetings_bytes,0\n"
            "42,InsertRemove.id-2.Remove_bytes,30\n"
            "45,InsertRemove.id-1.Remove_bytes,40\n"
            "30,InsertRemove.id-2.Insert_bytes,200\n"
            "28,InsertRemove.id-1.Insert_bytes,300\n"
            "26,HelloWorld.id-3.Greetings_docs,0\n"
            "42,InsertRemove.id-2.Remove_docs,7\n"
            "45,InsertRemove.id-1.Remove_docs,6\n"
            "30,InsertRemove.id-2.Insert_docs,8\n"
            "28,InsertRemove.id-1.Insert_docs,9\n"
            "26,HelloWorld.id-3.Greetings_iters,2\n"
            "42,InsertRemove.id-2.Remove_iters,1\n"
            "45,InsertRemove.id-1.Remove_iters,1\n"
            "30,InsertRemove.id-2.Insert_iters,1\n"
            "28,InsertRemove.id-1.Insert_iters,1\n"
            "\n"
            "Gauges\n"
            "\n"
            "Timers\n"
            "26,HelloWorld.id-3.Greetings_timer,13\n"
            "42,InsertRemove.id-2.Remove_timer,10\n"
            "45,InsertRemove.id-1.Remove_timer,17\n"
            "30,InsertRemove.id-2.Insert_timer,20\n"
            "28,InsertRemove.id-1.Insert_timer,23\n"
            "\n";

        std::ostringstream out;
        reporter.report<ReporterClockSourceStub>(out, "csv");
        REQUIRE(out.str() == expected);
    }
}

TEST_CASE("Genny.Setup metric should only be reported as a timer") {
    RegistryClockSourceStub::reset();
    auto metrics = v1::RegistryT<RegistryClockSourceStub>{};
    auto reporter = genny::metrics::v1::ReporterT{metrics};

    // Mimic what the DefaultDriver would be doing.
    auto setup = metrics.operation("Genny", "Setup", 0u);

    RegistryClockSourceStub::advance(5ns);
    auto ctx = setup.start();

    RegistryClockSourceStub::advance(10ns);
    ctx.success();

    auto expected =
        "Clocks\n"
        "SystemTime,42000000\n"
        "MetricsTime,15\n"
        "\n"
        "Counters\n"
        "\n"
        "Gauges\n"
        "\n"
        "Timers\n"
        "15,Genny.Setup,10\n"
        "\n";

    std::ostringstream out;
    reporter.report<ReporterClockSourceStub>(out, "csv");
    REQUIRE(out.str() == expected);
}

TEST_CASE("Genny.ActiveActors metric should be reported as a counter") {
    RegistryClockSourceStub::reset();
    auto metrics = v1::RegistryT<RegistryClockSourceStub>{};
    auto reporter = genny::metrics::v1::ReporterT{metrics};

    // Mimic what the DefaultDriver would be doing.
    auto startedActors = metrics.operation("Genny", "ActorStarted", 0u);
    auto finishedActors = metrics.operation("Genny", "ActorFinished", 0u);

    auto startActor = [&]() {
        auto ctx = startedActors.start();
        ctx.addDocuments(1);
        ctx.success();
    };

    auto finishActor = [&]() {
        auto ctx = finishedActors.start();
        ctx.addDocuments(1);
        ctx.success();
    };

    // Start 2 actors, have 1 finish, start 1 more, and have the remaining 2 finish.
    RegistryClockSourceStub::advance(5ns);
    startActor();
    RegistryClockSourceStub::advance(10ns);
    startActor();
    RegistryClockSourceStub::advance(20ns);
    finishActor();
    RegistryClockSourceStub::advance(50ns);
    startActor();
    RegistryClockSourceStub::advance(100ns);
    finishActor();
    RegistryClockSourceStub::advance(200ns);
    finishActor();

    auto expected =
        "Clocks\n"
        "SystemTime,42000000\n"
        "MetricsTime,385\n"
        "\n"
        "Counters\n"
        "5,Genny.ActiveActors,1\n"
        "15,Genny.ActiveActors,2\n"
        "35,Genny.ActiveActors,1\n"
        "85,Genny.ActiveActors,2\n"
        "185,Genny.ActiveActors,1\n"
        "385,Genny.ActiveActors,0\n"
        "\n"
        "Gauges\n"
        "\n"
        "Timers\n"
        "\n";

    std::ostringstream out;
    reporter.report<ReporterClockSourceStub>(out, "csv");
    REQUIRE(out.str() == expected);
}

}  // namespace
}  // namespace genny::metrics
