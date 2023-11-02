import matplotlib

matplotlib.use('tkagg')

import matplotlib.pyplot as plt
import numpy as np

from matplotlib import colors
from matplotlib.ticker import PercentFormatter
from pathlib import Path

import os, sys
from collections import defaultdict

# CONFIG=${1}
# VERSION_NO=${2}
# NUM_THREADS=${3}
# NUM_REQS=${4}
# PAYLOAD_SIZE=${5}
# WARMUP_PERIOD=${6}

config = sys.argv[1]
version = sys.argv[2]
num_threads = sys.argv[3]
num_reqs = sys.argv[4]
payload_size = sys.argv[5]
warmup_period = sys.argv[6]

filename_base = f'logs,{config},{version}/{num_threads},{num_reqs},{payload_size},{warmup_period}'

runtimes = defaultdict(0)
data = defaultdict(map)
flat_data = []

runs = 0

# try each run number

def get_data():
    global data
    global flat_data
    global runtimes
    global runs
    for runNo in range(99999):
        runtime = -1
        for threadNo in range(int(num_threads)):
            filename = filename_base + f",{threadNo},{runNo},.us"
            # TODO: fail gracefully
            path = Path(filename)
            if not path.exists():
                return
            my_data = map(int, path.read_text().strip().split("\n"))
            runtime = max(runtime, sum(my_data))
            #data[runNo][threadNo] = my_data
            flat_data += my_data
        runtimes[runNo] = runtime
        runs += 1

get_data()


colors = ['aqua', 'red', 'gold', 'royalblue', 'darkorange', 'green', 'purple', 'cyan', 'yellow', 'lime']

decades = np.arange(50, 200, 10)
# look at ALL data
fig, ax = plt.subplots()
ax.set_xlim(0, 200)
cnts, values, bars = ax.hist(flat_data, edgecolor='k', bins=decades)
#print(flat_data)
for i, (cnt, value, bar) in enumerate(zip(cnts, values, bars)):
    bar.set_facecolor(colors[i % len(colors)])
plt.show()
