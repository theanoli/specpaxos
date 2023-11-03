import matplotlib

#matplotlib.use('tkagg')

import matplotlib.pyplot as plt
import numpy as np

from matplotlib import colors
from matplotlib.ticker import PercentFormatter
import matplotlib.colors as mcolors
from pathlib import Path

import os, sys, mmap
import re
from collections import defaultdict

DURATION=30
# validate_reads, num_clients, threads_per_client

def make_filename(config, version, validate_reads, num_clients, threads_per_client, run_num, suffix='.us',**kwargs):
    return f'logs,{config},{version}/{validate_reads},{num_clients},{threads_per_client},{run_num},{suffix}'

def unmake_filename(filename):
    slashsplit = str(filename).split("/")
    commasplit0 = slashsplit[0].split(",")
    commasplit1 = slashsplit[1].split(",")
    assert commasplit0[0] == 'logs'
    assert commasplit1[4] == '.us'
    return {
            'config': commasplit0[1],
            'version': commasplit0[2],
            'validate_reads': commasplit1[0],
            'num_clients': commasplit1[1],
            'threads_per_client': commasplit1[2],
            'run_num': commasplit1[3],
    }

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

    return (sLatency, start, end, error)

def avg(a):
    a = list(a)
    return sum(a)/len(a)

def get_raw_data(supplied):
    keys = ["config", "version", "validate_reads", "num_clients", "threads_per_client", "run_num"]
    keys_needed = [key for key in keys if key not in supplied]
    #print(f"Finding: {keys_needed}")
    request = supplied.copy()
    for key in keys_needed:
        request[key] = "*"
    toglob = make_filename(**request, suffix=".us")
    paths = Path('.').glob(toglob)
    print(toglob)
    # now we need to parse all the results
    #results = map(paths, unmake_filename)
    # first, get the average latency, total runtime, anything else useful in each run
    data = []
    for path in paths:
        try:
            raw_data, start, end, error = process_file(path)
        except ValueError as e:
            print(f"Error reading file: {path}")
            raise e
        rt = end - start
        run = unmake_filename(path)
        run['raw data'] = raw_data
        run['average latency'] = avg(raw_data)
        run['total runtime'] = rt
        run['num reqs'] = len(raw_data)
        run['total thruput'] = len(raw_data) / rt
        run['error'] = error
        #"""
        if error == 'no' or error == 'maybe':
            # check for errors
            errfile = make_filename(**run, suffix=".stderr")
            # mmap it because it may be huge
            with open(errfile, 'r') as f:
                #print(errfile)
                #print(f)
                errtext = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
                state_err = re.search(b'PANIC|state transfer|received a duplicate message', errtext)
                if (state_err):
                    run['error'] = 'yes'
            if run['error'] == 'no':
                if os.path.getsize(errfile) > 1000000:
                    run['error'] = 'maybe'
                    #"""
        #run['thread thruput'] = int(run['payload_size']) * int(run['num_reqs']) / rt # TODO: this may need to be divided by num_threads
        data.append(run)
    return data
    

def graph(data, xprop, yprop, zprop_lambda, xlabel='', ylabel='', zlabel=''):
    # Assumption: all datas have the same config and version for one graph() call
    # get all the (x, y, z, error) pairs
    data4 = [(run[xprop], run[yprop], zprop_lambda(run), run['error']) for run in data if run['error'] != 'yes']
    # get all unique z layers
    #zlayers = sorted(list(set(run[2] for run in data4)), key=lambda x: float(x))
    zlayers = sorted(list(set(run[2] for run in data4)), key=lambda x: float(x))
    fig, ax = plt.subplots()
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    colors = mcolors.TABLEAU_COLORS
    for zlayer, color in zip(zlayers, colors):
        data3 = [run for run in data4 if run[2] == zlayer]
        data3 = sorted(data3, key=lambda x: float(x[0]))
        xs = [float(d[0]) for d in data3]
        ys = [float(d[1]) for d in data3]
        es = [(d[3]) for d in data3]
        p, = ax.plot(xs, ys, c=color)
        for (x, y, e) in zip(xs, ys, es):
            marker = 'o' if e == 'no' else 'x' if e == 'yes' else 'p'
            ax.plot(x,y,marker=marker, c=color,ms=4 if e=='no' else 8)
        p.set_label(zlayer)
        print(f"Plotting {xs} by {ys}")
    ax.set_ylim(bottom=0)
    ax.set_xlim(left=0)
    ax.legend(title=zlabel)
    # Assumption: all datas have the same config and version for one graph() call
    for run in data:
        plt.title(run['config'] + ", " + run['version'])
        break

    #plt.show()

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
    #time_ran = sys.argv[3]
    #payload_size = sys.argv[5]
    #warmup_period = sys.argv[4]
    output_to_file = len(sys.argv) == 4


    data = get_raw_data({'config': config, 'version': version})
    #print(data)
    graph(data, 'total thruput', 'average latency', lambda x: f"{x['threads_per_client']:>03}",
                'Throughput (ops/s)', 'Average Latency (us)', 'Threads per client')
    if output_to_file:
        plt.savefig(f'graphs/{config},{version},.png')
    else:
        plt.show()

if __name__ == '__main__':
    main()
