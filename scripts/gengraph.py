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


def graph(df, meta, xprop, yprop, zprop, xlabel='', ylabel='', zlabel='', title='', symbolProp=None, sortX=True, left=0, right=None, bottom=0, extra_title="", xscale="linear", yscale="linear", markersize=None, legendLoc="best"):
    # Assumption: all datas have the same config and version for one graph() call
    # get all the (x, y, z, error) pairs
    #data4 = [(run[xprop], run[yprop], zprop_lambda(run), run['error']) for run in data]
    # get all unique z layers
    #zlayers = sorted(list(set(run[2] for run in data4)), key=lambda x: float(x))
    if zprop is None:
        zprop = '_zee'
        zlabel = None
        df[zprop] = '0'
    #print('z props:', df[zprop].unique())
    zlayers = df[zprop].unique()
    fig, ax = plt.subplots()
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    colors = list(mcolors.TABLEAU_COLORS) + list(mcolors.XKCD_COLORS)
    dashings = ["-", "--"]
    symbols = ["o", "^", "v", "<", ">", "D", "s", "8", "*", "d"]
    zI = 0
    sI = 0
    symbolValue = None
    #for zlayer, color in zip(zlayers, colors):
    for zlayer in zlayers:
        data3Props = [xprop, yprop, 'error']
        if symbolProp is not None:
            data3Props += [symbolProp]
        data3 = df.loc[df[zprop] == zlayer][data3Props].values.tolist()
        if sortX:
            data3 = sorted(data3, key=lambda x: float(x[0]))
        #data3 = [run for run in data4 if run[2] == zlayer]
        #print(data3)
        xs = [float(d[0]) for d in data3]
        ys = [float(d[1]) for d in data3]
        es = [(d[2]) for d in data3]
        if symbolProp is not None:
            sV = [(d[3]) for d in data3]
            assert all(np.array(sV) == sV[0])
            if symbolValue is None:
                symbolValue = sV[0]
            elif sV[0] != symbolValue:
                print(f"mismatch in {sV[0]} and {symbolValue}")
                symbolValue = sV[0]
                zI += 1
                sI = 0
            else:
                sI += 1
            color = colors[zI]
            symbol = symbols[zI]
        else:
            color = colors[zI]
            symbol = symbols[zI] # potato
            zI += 1
        p, = ax.plot(xs, ys,  symbol + dashings[sI],c=color, markersize=markersize)
        for (x, y, e) in zip(xs, ys, es):
            if e != "no":
                marker = symbol if e == 'no' else 'x' if e == 'yes' else 'p'
                ax.plot(x,y,marker=marker, c=color,ms=4 if e=='no' else 10)
        if zlabel is not None:
            p.set_label(zlayer)
        if len(ys) < 300:
            print(f"Plotting {xs} by {ys}")
    if bottom is not None:
        ax.set_ylim(bottom=bottom)
    if left is not None:
        ax.set_xlim(left=left)
    if right is not None:
        ax.set_xlim(right=right)
    ax.set_xscale(xscale)
    ax.set_yscale(yscale)
    if zlabel is not None:
        ax.legend(title=zlabel, loc=legendLoc)
    # Assumption: all datas have the same config and version for one graph() call
    row0 = df.loc[0]
    if title == '':
        plt.title(row0['config'] + ", " + row0['version'])
    savename = ""
    for item in meta:
        # ignore things in the graph
        if item in [xprop, yprop, zprop]: continue
        if len(df[item].unique()) == 1:
            savename += f"{row0[item]},"
        else:
            savename += f"{item},"

    savename += f"{xprop} vs {yprop} vs {zprop},"
    if len(extra_title) != 0:
        savename += extra_title + ","
    #plt.show()
    plt.savefig("graphs/" + savename + ".png")
    plt.savefig("pdfs/" + savename + ".pdf")
    print(f"Saved graph to {savename}")

