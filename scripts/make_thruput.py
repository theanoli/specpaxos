import matplotlib

matplotlib.use('tkagg')

import matplotlib.pyplot as plt
import numpy as np

from matplotlib import colors
from matplotlib.ticker import PercentFormatter
from pathlib import Path

import os, sys
from collections import defaultdict


def make_filename(config, version, num_threads, num_reqs, payload_size, warmup_period, thread_num, run_num, **kwargs):
    return f'logs,{config},{version}/{num_threads},{num_reqs},{payload_size},{warmup_period},{thread_num},{run_num},.us'

def unmake_filename(filename):
    slashsplit = str(filename).split("/")
    commasplit0 = slashsplit[0].split(",")
    commasplit1 = slashsplit[1].split(",")
    assert commasplit0[0] == 'logs'
    assert commasplit1[6] == '.us'
    return {
            'config': commasplit0[1],
            'version': commasplit0[2],
            'num_threads': commasplit1[0],
            'num_reqs': commasplit1[1],
            'payload_size': commasplit1[2],
            'warmup_period': commasplit1[3],
            'thread_num': commasplit1[4],
            'run_num': commasplit1[5],
    }

def get_raw_data(supplied):
    keys = ["config", "version", "num_threads", "num_reqs", "payload_size", "warmup_period", "thread_num", "run_num"]
    keys_needed = [key for key in keys if key not in supplied]
    #print(f"Finding: {keys_needed}")
    request = supplied.copy()
    for key in keys_needed:
        request[key] = "*"
    toglob = make_filename(**request)
    paths = Path('.').glob(toglob)
    print(toglob)
    # now we need to parse all the results
    #results = map(paths, unmake_filename)
    # first, get the average latency, total runtime, anything else useful in each run
    data = []
    for path in paths:
        raw_data = list(map(int, Path(path).read_text().strip().split("\n")))
        rt = sum(raw_data)
        run = unmake_filename(path)
        run['raw data'] = raw_data
        run['average latency'] = rt / len(raw_data)
        run['thread runtime'] = rt
        #run['thread thruput'] = int(run['payload_size']) * int(run['num_reqs']) / rt # TODO: this may need to be divided by num_threads
        data.append(run)
    return data
    
def collapse_threads(data): # raw_data
    # next, collapse across each thread
    what_defines_a_run = ['config', 'version', 'num_threads', 'num_reqs', 'payload_size', 'warmup_period', 'run_num']
    # find all unique instances of that
    data_grouped_by_thread = []
    num_threads_normal = data[0]['num_threads']
    for run in data:
        if run['num_threads'] != num_threads_normal: print(f"Warning: run {make_filename(**run)} has {run['num_threads']} threads instead of {num_threads_normal}.")
        if (run['thread_num'] == '0'):
            data_grouped_by_thread.append({key: value for (key, value) in run.items() if key in what_defines_a_run})
    #print(data_grouped_by_thread)

    # now, collect the data with these new labels
    for run in data_grouped_by_thread:
        #data_with_my_attributes = [orun for orun in data if orun[a] == run[a] for a in what_defines_a_run]
        data_with_my_attributes = []
        for orun in data:
            if all(orun[prop] == run[prop] for prop in what_defines_a_run):
                data_with_my_attributes.append(orun)
        #print(f"for run {run} we get data {data_with_my_attributes}")
        #print(len(data_with_my_attributes))
        run['total runtime'] = max(x['thread runtime'] for x in data_with_my_attributes)
        run['total thruput'] = int(run['payload_size']) * int(run['num_reqs']) * int(run['num_threads']) / run['total runtime']
        # TODO: add thread0, thread1, etc data to run if needed
    return data_grouped_by_thread

def avg(a):
    a = list(a)
    return sum(a)/len(a)

def collapse_runs(data): # data_grouped_by_thread
    what_defines_a_run = ['config', 'version', 'num_threads', 'num_reqs', 'payload_size', 'warmup_period']
    # find all unique instances of that
    data_grouped_by_runs = []
    highest_run_num = 0
    for run in data:
        #if run['num_threads'] != num_threads_normal: print(f"Warning: run {make_filename(**run)} has {run['num_threads']} threads instead of {num_threads_normal}.")
        if int(run['run_num']) > highest_run_num: highest_run_num = int(run['run_num'])
        if (run['run_num'] == '0'):
            data_grouped_by_runs.append({key: value for (key, value) in run.items() if key in what_defines_a_run})

    # now, collect the data with these new labels
    for run in data_grouped_by_runs:
        data_with_my_attributes = []
        for orun in data:
            if all(orun[prop] == run[prop] for prop in what_defines_a_run):
                data_with_my_attributes.append(orun)
        #print(f"for run {run} we get data {data_with_my_attributes}")
        #print(len(data_with_my_attributes))
        if not any(int(x['run_num']) == highest_run_num for x in data_with_my_attributes): print(f"Warning: run {make_filename(**run)} has {run['num_runs']} runs instead of {highest_run_num}.")
        run['average runtime'] = avg(x['total runtime'] for x in data_with_my_attributes)
        run['average thruput'] = avg(x['total thruput'] for x in data_with_my_attributes)
        # TODO: add run0, run1, etc data to run if needed
    return data_grouped_by_runs

def graph(data, xprop, yprop, zprop):
    # get all the (x, y, z) pairs
    data3 = [(run[xprop], run[yprop], run[zprop]) for run in data]
    # get all unique z layers
    zlayers = sorted(list(set(run[2] for run in data3)), key=lambda x: float(x))
    fig, ax = plt.subplots()
    for zlayer in zlayers:
        data2 = [run[0:2] for run in data3 if run[2] == zlayer]
        data2 = sorted(data2, key=lambda x: float(x[0]))
        xs = [float(d[0]) for d in data2]
        ys = [float(d[1]) for d in data2]
        p, = ax.plot(xs, ys)
        p.set_label(zlayer)
        print(f"Plotting {xs} by {ys}")
    ax.legend()
    plt.show()

def main():
    # CONFIG=${1}
    # VERSION_NO=${2}
    # NUM_THREADS=${3}
    # NUM_REQS=${4}
    # PAYLOAD_SIZE=${5}
    # WARMUP_PERIOD=${6}

    config = sys.argv[1]
    version = sys.argv[2]
    #num_threads = sys.argv[3]
    num_reqs = sys.argv[3]
    #payload_size = sys.argv[5]
    warmup_period = sys.argv[4]


    data = get_raw_data({'config': config, 'version': version, 'num_reqs': num_reqs, 'warmup_period': warmup_period})
    #print(data)
    data = collapse_threads(data)
    #print(data)
    data = collapse_runs(data)
    print(data)
    graph(data, 'num_threads', 'average thruput', 'payload_size')

if __name__ == '__main__':
    main()

def __():
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
