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

from analysis import analyze
def main():
    config1 = sys.argv[1]
    version1 = sys.argv[2]
    config2 = sys.argv[3]
    version2 = sys.argv[4]
    config3 = sys.argv[5]
    version3 = sys.argv[6]

    TIME_RAN = 30000000000
    WARMUP_TIME = 10

    output = False

    df1, m1 = analyze(config1, version1, TIME_RAN, WARMUP_TIME, output=output)
    df2, m2 = analyze(config2, version2, TIME_RAN, WARMUP_TIME, output=output)
    df3, m3 = analyze(config3, version3, TIME_RAN, WARMUP_TIME, output=output)

    options = [32, 1024]
    df1 = df1.loc[df1['payload_size'].isin(options)]
    df2 = df2.loc[df2['payload_size'].isin(options)]
    df3 = df3.loc[df3['payload_size'].isin(options)]

    df1["source"] = "CPU Beehive Off"
    df2["source"] = "CPU Beehive On"
    df3["source"] = "FPGA"

    df1["source + payload size"] = df1["source"] + " - " + df1["payload_size"].apply(str)
    df2["source + payload size"] = df2["source"] + " - " + df2["payload_size"].apply(str)
    df3["source + payload size"] = df3["source"] + " - " + df3["payload_size"].apply(str)

    #df = pd.concat([df1, df2, df3]).reset_index(drop=True)
    df = pd.concat([df2, df3]).reset_index(drop=True)

    graph(df, m1,   'num_threads', 'average ops|sec', 'source + payload size', 
                    'Num Clients', 'Operations/sec (ops/s)', 'Payload Size (B)', symbolProp='source',sortX=False)


if __name__ == '__main__':
    main()
