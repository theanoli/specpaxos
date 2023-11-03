from gengraph import graph
from fileio import FileIO, collapse
import numpy as np
import pandas as pd

from pathlib import Path

import os, sys, mmap
import re
from collections import defaultdict

def analyze(config, version, time_ran, warmup_period, output=True):
    
    fio = FileIO('logs,{config},{version}/{num_threads},{time_ran},{payload_size},{warmup_period},{thread_num},{run_num},{suffix}')

    df, meta = fio.get_raw_data({'config': config, 'version': version, 'time_ran': time_ran, 'warmup_period': warmup_period}, lambda path: {"raw data": list(map(int, Path(path).read_text().strip().split("\n")))})
    df, meta = collapse(df, meta, ['warmup_period', 'time_ran'], None)

    df["thread runtime"] = df["raw data"].apply(sum)
    df["num reqs"] = df["raw data"].apply(len)
    df["average thread latency"] = df["thread runtime"] / df["num reqs"]
    #print(df.head())

    df, meta = collapse(df, meta, ['thread_num'], {'total num reqs': ('num reqs', 'sum'),
                                                   'total runtime' : ('thread runtime', 'max')})
    df['total thruput'] = df['payload_size'] * df['total num reqs'] / df['total runtime']
    df['total ops|sec'] = 1000000            * df['total num reqs'] / df['total runtime']
    #print(df.head())
    df['payload size + run number'] = df['payload_size'].map(str) + '.' + df['run_num'].map(str)

    # if there are any duplicate runs
    if any(df['run_num'] == 1):
        if output:
            graph(df, meta, 'num_threads', 'total thruput', 'payload size + run number',
                            'Num Clients', 'Throughput (MB/s)', 'Payload Size.Run Number')

    df, meta = collapse(df, meta, ['run_num'], 
            {'average runtime': ('total runtime', 'mean'),
             'average thruput': ('total thruput', 'mean'),
             'average ops|sec': ('total ops|sec', 'mean')})

    if output:
        print(df.head())

        graph(df, meta, 'num_threads', 'average thruput', 'payload_size',
                        'Num Clients', 'Throughput (MB/s)', 'Payload Size (B)')

        graph(df, meta, 'num_threads', 'average ops|sec', 'payload_size',
                        'Num Clients', 'Operations/sec (ops/s)', 'Payload Size (B)')
    return df, meta

def main():
    config = sys.argv[1]
    version = sys.argv[2]
    #num_threads = sys.argv[3]
    time_ran = sys.argv[3]
    #payload_size = sys.argv[5]
    warmup_period = sys.argv[4]
    analyze(config, version, time_ran, warmup_period)

if __name__ == '__main__':
    main()
