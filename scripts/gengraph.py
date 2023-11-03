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


def graph(df, meta, xprop, yprop, zprop, xlabel='', ylabel='', zlabel='', title='', sortX=True, connectZ=False):
    # Assumption: all datas have the same config and version for one graph() call
    # get all the (x, y, z, error) pairs
    #data4 = [(run[xprop], run[yprop], zprop_lambda(run), run['error']) for run in data]
    # get all unique z layers
    #zlayers = sorted(list(set(run[2] for run in data4)), key=lambda x: float(x))
    zlayers = sorted(df[zprop].unique())
    fig, ax = plt.subplots()
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    colors = mcolors.TABLEAU_COLORS
    for zlayer, color in zip(zlayers, colors):
        if sortX:
            data3 = sorted(df.loc[df[zprop] == zlayer][[xprop, yprop, 'error']].values.tolist(), key=lambda x: float(x[0]))
        else:
            data3 = df.loc[df[zprop] == zlayer][[xprop, yprop, 'error']].values.tolist()
        #data3 = [run for run in data4 if run[2] == zlayer]
        #print(data3)
        xs = [float(d[0]) for d in data3]
        ys = [float(d[1]) for d in data3]
        es = [(d[2]) for d in data3]
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
    row0 = df.loc[0]
    if title == '':
        plt.title(row0['config'] + ", " + row0['version'])
    savename = "graphs/"
    for item in meta:
        # ignore things in the graph
        if item in [xprop, yprop, zprop]: continue
        if len(df[item].unique()) == 1:
            savename += f"{row0[item]},"
        else:
            savename += f"{item},"

    savename += f"{xprop}@{yprop}@{zprop},"
    savename += '.png'
    #plt.show()
    plt.savefig(savename)
    print(f"Saved graph to {savename}")

