import copy
import matplotlib
import pandas
import warnings
import scipy
from scipy import stats
import subprocess
import numpy as np
import numpy
from matplotlib import pyplot as plt
import json
import seaborn
import networkx as nx
import os
import argparse

def curate_file(file):
    jsonfile = '.'.join(file.split(".")[:-1] + ["json"]).split("/")[::-1][0]
    if(os.path.exists(jsonfile)):
        os.remove(jsonfile)
    subprocess.getoutput("curator ftdc export json --input " + file + " > " + jsonfile)
    return jsonfile

def make_differential_frame(df, dx):
    b = pandas.DataFrame()
    b[dx] = df[dx]
    b["d(t_pure)"] = df["d(t_pure)"]
    b["d(t_total)"] = df["d(t_total)"]
    b["d(t_overhead)"] = df["d(t_overhead)"]

    b["total_latency"] = b["d(t_total)"] / b[dx]
    b["overhead_latency"] = b["d(t_overhead)"] / b[dx]
    b["pure_latency"] = b["d(t_pure)"] / b[dx]
    b["ts"] = df["ts"]
    b["actor_id"] = df["actor_id"]

    # Have a single row for every sample/increment
    b = b.loc[b.index.repeat(b[dx])]

    return b

# Given an ftdc file generates the data necessary for analysis
def get_data(file):
    jsonfile = curate_file(file)
    data = {
        "id": [],
        "counters.n": [],
        "counters.ops": [],
        "counters.size": [],
        "counters.errors": [],
        "timers.dur": [],
        "timers.total": [],
        "gauges.state": [],
        "gauges.workers": [],
        "gauges.failed": [],
        "ts":[]
    }
    with open(jsonfile) as f:
        while (line := f.readline()) != "":
            loaded = json.loads(line)
            data["id"].append(loaded["id"])
            data["counters.n"].append(loaded["counters"]["n"])
            data["counters.ops"].append(loaded["counters"]["ops"])
            data["counters.size"].append(loaded["counters"]["size"])
            data["counters.errors"].append(loaded["counters"]["errors"])
            data["timers.dur"].append(loaded["timers"]["dur"])
            data["timers.total"].append(loaded["timers"]["total"])
            data["gauges.state"].append(loaded["gauges"]["state"])
            data["gauges.workers"].append(loaded["gauges"]["workers"])
            data["gauges.failed"].append(loaded["gauges"]["failed"])
            data["ts"].append(loaded["ts"])
    raw_data = pandas.DataFrame(data)
    new_row = pandas.DataFrame({
        "id": 0,
        "counters.n": 0,
        "counters.ops": 0,
        "counters.size": 0,
        "counters.errors": 0,
        "timers.dur": 0,
        "timers.total": 0,
        "gauges.state": 0,
        "gauges.workers": 0,
        "gauges.failed": 0,
    }, index=[-1])
    intermediate = pandas.concat([new_row, raw_data])

    fixed_data = pandas.DataFrame()
    fixed_data["actor_id"] = intermediate["id"]
    fixed_data["d(n)"] = intermediate["counters.n"].diff()
    fixed_data["d(ops)"] = intermediate["counters.ops"].diff()
    fixed_data["d(size)"] = intermediate["counters.size"].diff()
    fixed_data["d(err)"] = intermediate["counters.errors"].diff()
    fixed_data["d(t_pure)"] = intermediate["timers.dur"].diff()
    fixed_data["d(t_total)"] = intermediate["timers.total"].diff()
    fixed_data["d(t_overhead)"] = fixed_data["d(t_total)"] - fixed_data["d(t_pure)"]
    fixed_data["ts"] = pandas.to_datetime(intermediate["ts"], unit="ms")

    fixed_data = fixed_data.loc[0:]
    b = make_differential_frame(fixed_data, "d(ops)")
    return fixed_data, b, intermediate.loc[0:]

def processDirResults(dir):
    totalMoveChunkFilepath = dir + "MoveChunk.totalMoveChunk.ftdc"
    totalCriticalSectionTimeMillisFilepath = dir + "MoveChunk.totalCriticalSectionTimeMillis.ftdc"
    totalCriticalSectionCommitTimeMillisFilepath = dir + "MoveChunk.totalCriticalSectionCommitTimeMillis.ftdc"
    totalRecipientCriticalSectionTimeMillisFilepath = dir + "MoveChunk.totalRecipientCriticalSectionTimeMillis.ftdc"

    _, donorCriticalSectionDf, _ = get_data(totalCriticalSectionTimeMillisFilepath)
    _, donorCriticalSectionCommitDf, _ = get_data(totalCriticalSectionCommitTimeMillisFilepath)
    _, donorRecipientCriticalSectionDf, _ = get_data(totalRecipientCriticalSectionTimeMillisFilepath)

    csTimes = pandas.DataFrame()
    csTimes["donor_CS_total_ms"] = donorCriticalSectionDf['pure_latency']/1000
    csTimes["donor_CS_commit_ms"] = donorCriticalSectionCommitDf['pure_latency']/1000
    csTimes["recipient_CS_total_ms"] = donorRecipientCriticalSectionDf['pure_latency']/1000

    return csTimes

# Test results
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Takes one or more 'CedarResults/' dir(s) and analyzes (and compares) the "
                    "migration critical section durations")
    parser.add_argument('dir', metavar='dir', type=str, nargs='+',
                        help='input CedarMetrics dir(s)')
    args = parser.parse_args()

    # Process experiment data
    experiment_num = 0
    experiments = {}
    for dir in args.dir:
        csTimes = processDirResults(dir)
        experiment_id = chr(ord('A') + experiment_num)
        csTimes['experiment'] = experiment_id
        experiments[experiment_id] = csTimes
        experiment_num += 1

    # For each experiment, print statistics summary
    for experiment_id in experiments:
        print("-- Experiment " + experiment_id + "--")
        print(experiments[experiment_id].describe())
        print("")
        experiments[experiment_id].describe().to_csv("describe_experiment_" + experiment_id + ".csv")

    # Boxplot experiments
    csTimes_concat = pandas.concat(experiments.values())
    bplot = csTimes_concat.boxplot(
        by='experiment',
        column=['donor_CS_total_ms', 'donor_CS_commit_ms', 'recipient_CS_total_ms'],
        layout=(1,3),
        figsize=(10,10))
    bplot[0].set_ylabel('ms')
    plt.suptitle("sys-perf \"Linux 3-Shard Cluster\"; 1000 chunks; 0 documents;no CRUD load;\nA := featureFlagMigrationRecipientCriticalSection off, B:= featureFlagMigrationRecipientCriticalSection on")
    plt.savefig("cs_boxplots_compare.png")

