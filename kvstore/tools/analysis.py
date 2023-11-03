import os, sys, mmap
sys.path.append(os.getcwd() + '/../../scripts')
from gengraph import graph
from fileio import FileIO, collapse
import numpy as np
import pandas as pd

from pathlib import Path

import re
from collections import defaultdict

DURATION = 30

def process_file(filepath):
    start, end = -1.0, -1.0

    duration = float(DURATION)
    warmup = duration/3.0

    tLatency = []
    sLatency = []
    fLatency = []

    tExtra = 0.0
    sExtra = 0.0
    fExtra = 0.0

    xLatency = []
    error = 'no'

    for line in open(filepath):
        if line.startswith('#') or line.strip() == "":
            continue

        line = line.strip().split()
        if not line[0].isdigit() or len(line) < 4 or re.search("[a-zA-Z]", "".join(line)):
            continue

        if start == -1:
            start = float(line[2]) + warmup
            end = start + warmup

        fts = float(line[2])
      
        if fts < start:
            continue

        if fts > end:
            break

        latency = int(line[3])
        status = int(line[4])
        ttype = -1
        try:
            ttype = int(line[5])
            extra = int(line[6])
        except:
            extra = 0

        if status == 1 and ttype == 2:
            xLatency.append(latency)

        tLatency.append(latency) 
        tExtra += extra
        if status == 1:
            sLatency.append(latency)
            sExtra += extra
        else:
            fLatency.append(latency)
            fExtra += extra

    if len(tLatency) == 0:
        print("Zero completed transactions..")
        #sys.exit()
        error = 'yes'

    tLatency.sort()
    sLatency.sort()
    fLatency.sort()

    return {"raw data": sLatency, "start": start, "end": end, "error": error, "file": filepath}


def analyze(config, version, output=True):
    fio = FileIO('logs,{config},{version}/{validate_reads},{num_clients},{threads_per_client},{run_num},{suffix}')

    df, meta = fio.get_raw_data({'config': config, 'version': version}, process_file)

    df["total runtime"] = df["end"] - df["start"]
    df["average latency"] = df["raw data"].apply(np.mean)
    df["num reqs"] = df["raw data"].apply(len)
    df["total ops|sec"] = df["num reqs"] / df["total runtime"]
    if output:
        print(df.head())
        graph(df, meta, 'total ops|sec', 'average latency', 'threads_per_client',
                        'Throughput (ops/s)', 'Average Latency (us)', 'Threads per client')

    # average the throughput and latency
    df, meta = collapse(df, meta, ['run_num'], 
            {'average runtime': ('total runtime', 'mean'),
             'average average latency': ('average latency', 'mean'),
             'average ops|sec': ('total ops|sec', 'mean')})

    # before graphing, sort by threads_per_client to make the graph look nice
    df = df.sort_values("threads_per_client")
    if output:
        graph(df, meta, 'average ops|sec', 'average average latency', 'num_clients',
                        'Average Throughput (ops/s)', 'Average Latency (us)', 'Num hosts', sortX=False)
    return df, meta


def main(output=True):
    config = sys.argv[1]
    version = sys.argv[2]
    #num_threads = sys.argv[3]
    #time_ran = sys.argv[3]
    #payload_size = sys.argv[5]
    #warmup_period = sys.argv[4]
    analyze(config, version)

if __name__ == '__main__':
    main()
