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
    config4 = sys.argv[7]
    version4 = sys.argv[8]

    df1, m1 = analyze(config1, version1)
    df2, m2 = analyze(config2, version2)
    df3, m3 = analyze(config3, version3)
    df4, m4 = analyze(config4, version4)

    df1["source"] = "CPU RV=false"
    df2["source"] = "CPU RV=true"
    df3["source"] = "FPGA RV=false"
    df4["source"] = "FPGA RV=true"

    df = pd.concat([df1, df2, df3, df4]).reset_index(drop=True)

    graph(df, m1,   'average ops|sec', 'average median latency', 'source',
                    'Average Throughput (ops/s)', 'Average Median Latency (us)', 'Source', sortX=False)


if __name__ == '__main__':
    main()
