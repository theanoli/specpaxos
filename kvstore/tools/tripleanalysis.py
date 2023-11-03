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

    df1, m1 = analyze(config1, version1)
    df2, m2 = analyze(config2, version2)
    df3, m3 = analyze(config3, version3)

    df1["source"] = "CPU Beehive Off"
    df2["source"] = "CPU Beehive On"
    df3["source"] = "FPGA"

    df = pd.concat([df1, df2, df3]).reset_index(drop=True)

    graph(df, m1,   'average ops|sec', 'average average latency', 'source',
                    'Average Throughput (ops/s)', 'Average Latency (us)', 'Source', sortX=False)


if __name__ == '__main__':
    main()
