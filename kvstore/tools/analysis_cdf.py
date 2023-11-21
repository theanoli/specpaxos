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

def analyze_cdf(config, version, output=True, threads_per_client=1):
    fio = FileIO('logs,{config},{version}/{validate_reads},{num_clients},{threads_per_client},{run_num},{suffix}')

    df, meta = fio.get_raw_data({'config': config, 'version': version, 'run_num': 0, 'validate_reads': 'true'}, process_file)
    #df, meta = fio.get_raw_data({'config': config, 'version': version, 'run_num': 0}, process_file)

    #print(meta)
    #print(df.head())
    df = df.loc[df['threads_per_client'] == threads_per_client]
    #print(df.head())
    df = df.explode(["raw data", "read writes"], ignore_index=True)
    #print(df.head())
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

def main(output=True):
    config = sys.argv[1]
    version = sys.argv[2]
    #num_threads = sys.argv[3]
    #time_ran = sys.argv[3]
    #payload_size = sys.argv[5]
    #warmup_period = sys.argv[4]
    df, meta = analyze_cdf(config, version)

    right = calcRight(df, 0.99)
    print(right)
    graph(df.loc[df["variable"].isin(["reads", "writes"])], meta, 'raw data', 'value', 'variable',
                    'Latency (us)', 'CDF', 'Variable', left=85, right=right, markersize=0)
    left = calcRight(df, 0.95, "left")
    df = df.loc[df["value"] >= 0.95].reset_index()
    print(df.head())
    graph(df.loc[df["variable"].isin(["reads", "writes"])], meta, 'raw data', 'value', 'variable',
                        'Latency (us) (log scale)', 'CDF', 'Variable', bottom=None, left=left, xscale="log", extra_title="zoomed")

def main2():
    config2 = sys.argv[1]
    version2 = sys.argv[2]
    config3 = sys.argv[3]
    version3 = sys.argv[4]

    #df1, m1 = analyze_cdf(config1, version1)
    df2, m2 = analyze_cdf(config2, version2)
    df3, m3 = analyze_cdf(config3, version3)

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
                        #'Latency (us)', 'CDF', 'Device', sortX=False, markersize=0, left=10)
    # zoomed in graph
    left = min(map(lambda x: calcRight(x, 0.95, "left"), [df2, df3]))
    df = df.loc[df["value"] >= 0.95].reset_index(drop=True)
    print(df.head())
    graph(df, m2, 'raw data', 'value', 'source',
                        'Latency (us) (log scale)', 'CDF', 'Device', sortX=False, bottom=None, left=left, xscale="log", extra_title="zoomed")


def across_clients():
    config = sys.argv[1]
    version = sys.argv[2]

    df = None
    right = None

    for threads_per_client in [4, 8, 16, 32]:
        df_new, m = analyze_cdf(config, version, threads_per_client=threads_per_client)
        df_new = df_new.loc[df_new["variable"] == "ops"].reset_index(drop=True)
        right_new = calcRight(df_new, 0.99)
        if df is None:
            df = df_new
            right = right_new
        else:
            df = pd.concat([df, df_new]).reset_index(drop=True)
            right = max(right, right_new)

    graph(df, m, 'raw data', 'value', 'threads_per_client',
                        'Latency (us),' 'CDF', 'Threads per client', sortX=False, right=right, markersize=0)


if __name__ == '__main__':
    #main()
    main2()
    #across_clients()
