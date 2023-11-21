import matplotlib

#matplotlib.use('tkagg')

import matplotlib.pyplot as plt
import numpy as np
import seaborn as sns


from matplotlib import colors
from matplotlib.ticker import PercentFormatter
import matplotlib.colors as mcolors
from pathlib import Path

import os, sys, mmap
import re
from collections import defaultdict


def graph(df, meta, xprop, yprop, zprop, xlabel='', ylabel='', zlabel='', title='', symbolProp=None, sortX=True, left=0, right=None, bottom=0, extra_title="", xscale="linear", yscale="linear", markersize=None, legendLoc="best", extraTop=0, extraRight=0, ncols=1, image_size=None, font_sizes=[10, 14, 18]):
    # Assumption: all datas have the same config and version for one graph() call
    SMALL_SIZE = font_sizes[0]
    MEDIUM_SIZE = font_sizes[1]
    BIGGER_SIZE = font_sizes[2]

    plt.rc('font', size=SMALL_SIZE)          # controls default text sizes
    plt.rc('axes', titlesize=SMALL_SIZE)     # fontsize of the axes title
    plt.rc('axes', labelsize=MEDIUM_SIZE)    # fontsize of the x and y labels
    plt.rc('xtick', labelsize=SMALL_SIZE)    # fontsize of the tick labels
    plt.rc('ytick', labelsize=SMALL_SIZE)    # fontsize of the tick labels
    plt.rc('legend', fontsize=SMALL_SIZE)    # legend fontsize
    plt.rc('figure', titlesize=BIGGER_SIZE)  # fontsize of the figure title
    # get all the (x, y, z, error) pairs
    #data4 = [(run[xprop], run[yprop], zprop_lambda(run), run['error']) for run in data]
    # get all unique z layers
    #zlayers = sorted(list(set(run[2] for run in data4)), key=lambda x: float(x))
    df = df.reset_index(drop=True)
    if zprop is None:
        zprop = '_zee'
        zlabel = None
        df[zprop] = '0'
    #print('z props:', df[zprop].unique())
    zlayers = df[zprop].unique()
    fig, ax = plt.subplots()
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    #colors = list(mcolors.TABLEAU_COLORS) + list(mcolors.XKCD_COLORS)
    colors = sns.cubehelix_palette(n_colors=10, start=5, rot=4, hue=3, light=0.4, dark=.8)
    colors = np.array(colors)[[0,3,4,5,6]]
    dashings = ["-", "--"]
    markerfacecolors=[None, 'none']
    #symbols = ["o", "^", "v", "<", ">", "D", "s", "8", "*", "d"]
    symbols = ["o", "^", "D", "s", "*", "v", "<", ">", "8", "d"]
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
        p, = ax.plot(xs, ys,  symbol + dashings[sI], c=color, markersize=markersize, markerfacecolor=markerfacecolors[sI])
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
    ylim = ax.get_ylim()
    ax.set_ylim(ylim[0], ylim[1] + extraTop)
    xlim = ax.get_xlim()
    ax.set_xlim(xlim[0], xlim[1] + extraRight)
    ax.set_xscale(xscale)
    ax.set_yscale(yscale)
    if zlabel is not None:
        ax.legend(title=zlabel, loc=legendLoc, ncols=ncols)
    # Assumption: all datas have the same config and version for one graph() call
    row0 = df.loc[0]
    if title == '':
        plt.title(row0['config'] + ", " + row0['version'], fontsize=BIGGER_SIZE)
    else:
        plt.title(title, fontsize=BIGGER_SIZE)
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
    if image_size is not None:
        fig.set_size_inches(*image_size)
    plt.savefig("graphs/" + savename + ".png")
    plt.savefig("pdfs/" + savename + ".pdf")
    print(f"Saved graph to {savename}")
    return savename

