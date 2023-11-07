from analysis import main as process

import os, sys, mmap
sys.path.append(os.getcwd() + '/../../scripts')
from gengraph import graph
from fileio import FileIO, collapse
import numpy as np
import pandas as pd

from pathlib import Path

import re
from collections import defaultdict
from analysis import process_file

DURATION = 30

def analyze_cdf(config, version, output=True):
    fio = FileIO('logs,{config},{version}/{validate_reads},{num_clients},{threads_per_client},{run_num},{suffix}')

    df, meta = fio.get_raw_data({'config': config, 'version': version, 'run_num': 0}, process_file)

    df = df.loc[df['threads_per_client'] == 1]
    df = df.explode(["raw data", "read writes"], ignore_index=True)

    df = df.sort_values(["read writes", "raw data"]).reset_index(drop=True)
    num_rw = df["read writes"].value_counts()
    numer = -num_rw[0] * df["read writes"] + df.index + 1
    denom = num_rw[df["read writes"]].reset_index(drop=True)
    df["rw cdf"] = numer / denom


    df = df.sort_values("raw data").reset_index(drop=True)

    df["cdf"] = (df.index + 1) / len(df)

    if output:
        print(df.head())
        graph(df, meta, 'raw data', 'cdf', None,
                        'Latency (us)', 'CDF', '', sortX=False, markersize=0)
        graph(df, meta, 'raw data', 'rw cdf', 'read writes',
                        'Latency (us)', 'CDF', 'Read/Writes', sortX=False, markersize=0)

    return df, meta

def calcRight(df, cutoff):
    return df.iloc[(df.shape[0] * cutoff) // 100]["raw data"]

def main(output=True):
    config = sys.argv[1]
    version = sys.argv[2]
    #num_threads = sys.argv[3]
    #time_ran = sys.argv[3]
    #payload_size = sys.argv[5]
    #warmup_period = sys.argv[4]
    analyze_cdf(config, version)


def main2():
    config2 = sys.argv[1]
    version2 = sys.argv[2]
    config3 = sys.argv[3]
    version3 = sys.argv[4]

    #df1, m1 = analyze_cdf(config1, version1)
    df2, m2 = analyze_cdf(config2, version2)
    df3, m3 = analyze_cdf(config3, version3)

    #df1["source"] = "CPU Beehive Off"
    df2["source"] = "FPGA RV=false"
    df3["source"] = "FPGA RV=true"

    #df = pd.concat([df1, df2, df3]).reset_index(drop=True)
    df = pd.concat([df2, df3]).reset_index(drop=True)

    right = max(map(lambda x: calcRight(x, 99), [df2, df3]))

    # Calculate the 99th percentile
    #percentile_99 = df['raw data'].quantile(0.99)

    # Find the nearest value to the 99th percentile
    #nearest_index = (df['raw data'] - percentile_99).abs().idxmin()
    #nearest_value = df.loc[nearest_index, 'raw data']


    graph(df, m2, 'raw data', 'cdf', 'source',
                        'Latency (us)', 'CDF', 'Device', sortX=False, right=right, markersize=0)

    # zoomed in graph
    df = df.loc[df["cdf"] >= 0.95].reset_index()
    left = min(map(lambda x: calcRight(x, 95), [df2, df3]))
    print(df.head())
    graph(df, m2, 'raw data', 'cdf', 'source',
                        'Latency (us) (log scale)', 'CDF', 'Device', sortX=False, bottom=None, left=left, xscale="log", extra_title="zoomed")


if __name__ == '__main__':
    main()
