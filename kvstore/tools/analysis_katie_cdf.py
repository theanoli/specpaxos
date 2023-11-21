from analysis_kvstore import main as process

import os, sys, mmap
sys.path.append(os.getcwd() + '/../../scripts')
from gengraph import graph
from fileio import FileIO, collapse
import numpy as np
import pandas as pd

from pathlib import Path

import re
from collections import defaultdict
from analysis_kvstore import process_file

DURATION = 30

def analyze_cdf(config, version, pathstring, output=True):
    fio = FileIO(pathstring)

    df, meta = fio.get_raw_data({'config': config, 'version': version, 'shards': 4, 'clientnodes': 2, 'threads': 2}, process_file, suffix=".log", check_errfile=False)
    #df, meta = fio.get_raw_data({'config': config, 'version': version, 'run_num': 0}, process_file)

    df = df.explode(["raw data", "read writes"], ignore_index=True)
    meta += ["raw data", "read writes"]

    df['read'] = df['read writes'] == 0
    df['write'] = df['read writes'] == 1
    df['ops'] = 1
    
    #print(df.head())
    df, meta = collapse(df, meta, ["read writes"], {'delta reads': ('read', 'sum'),
                                                    'delta writes': ('write', 'sum'),
                                                    'delta ops': ('ops', 'sum')})
    #print(df.head())
    df[["cum reads", "cum writes", "cum ops"]] = df[["delta reads", "delta writes", "delta ops"]].cumsum()
    #print(df.head())

    cumtotals = df.tail(1)[["cum reads", "cum writes", "cum ops"]].values

    df[["reads", "writes", "ops"]] = df[["cum reads", "cum writes", "cum ops"]] / cumtotals

    #print(df.head())
    #df = df.melt(meta + ["error"], ["cum reads", "cum writes", "cum ops"])
    df = df.melt(meta + ["error"], ["reads", "writes", "ops"])

    #print(df.head())
    #print(df.sort_values("raw data"))

    if output:
        print(df.head())
        graph(df, meta, 'raw data', 'value', 'variable',
                        'Latency (us)', 'CDF', 'Variable', markersize=0)
    return df, meta

def calcRight(df, cutoff, direction="right"):
    #return df.iloc[(df.shape[0] * cutoff) // 100]["raw data"]
    rightI = df.loc[df["variable"] == "ops"]["value"].searchsorted(cutoff, "left" if direction == "right" else "right")
    right = df.loc[df["variable"] == "ops"].iloc[rightI]["raw data"]
    return right

def main2():
    #pathstring = '/home/katie/apiary/beehive-electrode/specpaxos-mod/{version}/{config}_{shards}shard_{clientnodes}clientnodes_{threads}threads/client{suffix}'
    pathstring = '/scratch/katie/{version}/{config}_{shards}shard_{clientnodes}clientnodes_{threads}threads/client{suffix}'
    config2 = sys.argv[1]
    version2 = sys.argv[2]
    config3 = sys.argv[3]
    version3 = sys.argv[4]

    #df1, m1 = analyze_cdf(config1, version1)
    df2, m2 = analyze_cdf(config2, version2, pathstring)
    df3, m3 = analyze_cdf(config3, version3, pathstring)

    #df1["source"] = "CPU Beehive Off"
    df2["source"] = "CPU"
    df3["source"] = "FPGA"

    #df = pd.concat([df1, df2, df3]).reset_index(drop=True)
    df = pd.concat([df2, df3]).reset_index(drop=True)
    df = df.loc[df["variable"] == "ops"].reset_index(drop=True)

    rightL = list(map(lambda x: calcRight(x, 0.99), [df2, df3]))
    print(rightL)
    right = max(rightL)

    graph(df, m2, 'raw data', 'value', 'source',
                        'Latency (us)', 'CDF', 'Device', sortX=False, right=right, markersize=0)

    #graph(df, m2, 'raw data', 'value', 'source',
                        #'Latency (us) (log scale)', 'CDF', 'Device', sortX=False, markersize=0, xscale="log", extra_title="log", left=10)
    # zoomed in graph
    left = min(map(lambda x: calcRight(x, 0.95, "left"), [df2, df3]))
    df = df.loc[df["value"] >= 0.95].reset_index(drop=True)
    print(df.head())
    graph(df, m2, 'raw data', 'value', 'source',
                        'Latency (us) (log scale)', 'CDF', 'Device', sortX=False, bottom=None, left=left, xscale="log", extra_title="zoomed")


if __name__ == '__main__':
    #main()
    main2()
