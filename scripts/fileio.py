import numpy as np
import pandas as pd

from pathlib import Path

import os, sys, mmap
import re
from collections import defaultdict

# src: https://stackoverflow.com/questions/11844986/convert-or-unformat-a-string-to-variables-like-format-but-in-reverse-in-p
def string_to_dict(string, pattern):
    regex = re.sub(r'{(.+?)}', r'(?P<_\1>.+)', pattern)
    values = list(re.search(regex, string).groups())
    keys = re.findall(r'{(.+?)}', pattern)
    _dict = dict(zip(keys, values))
    return _dict

def combine_errors(es):
    es = np.array(es)
    return "yes" if any(es == "yes") else "maybe" if any(es == "maybe") else "no"

class FileIO:
    def __init__(self, runstring):
        self.runstring = runstring
        braces = re.findall(r'\{.*?\}', runstring)
        braces.remove("{suffix}")
        self.what_defines_a_run = [x[1:-1] for x in braces]
        pass
    def make_filename(self, **kwargs):
        return self.runstring.format(**kwargs)
    def unmake_filename(self, filename):
        ret = string_to_dict(str(filename), self.runstring)
        del ret['suffix']
        return ret
    def get_raw_data(self, supplied, process_file):
        keys_needed = [key for key in self.what_defines_a_run if key not in supplied]
        #print(f"Finding: {keys_needed}")
        request = supplied.copy()
        for key in keys_needed:
            request[key] = "*"
        toglob = self.make_filename(**request, suffix=".us")
        paths = Path('.').glob(toglob)
        print(toglob)
        # now we need to parse all the results
        #results = map(paths, unmake_filename)
        #df = pd.DataFrame(columns = self.what_defines_a_run + ['raw data', 'error'])
        df = None
        for path in paths:
            try:
                ret_dict = process_file(path)
            except ValueError as e:
                print(f"Error reading file: {path}")
                raise e
            run = self.unmake_filename(path)
            # check for errors
            # mmap it because it may be huge
            errors = "no"
            if 'thread_num' not in run.keys() or run['thread_num'] == 0:
                errfile = self.make_filename(**run, suffix=".stderr")
                if os.path.getsize(errfile) > 1000000:
                    errors = 'maybe'
                with open(errfile, 'r') as f:
                    errtext = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
                    state_err = re.search(b'PANIC|state transfer|received a duplicate message', errtext)
                    if (state_err):
                        errors = 'yes'
            if "error" not in ret_dict:
                ret_dict["error"] = errors
            else:
                ret_dict["error"] = combine_errors([errors, ret_dict["error"]])
            if df is None:
                headers = self.what_defines_a_run + list(ret_dict.keys())
                df = pd.DataFrame(columns = headers)
            df.loc[len(df)] = {**run, **ret_dict}
        df = df.apply(pd.to_numeric, errors='ignore')
        return (df, self.what_defines_a_run)

def collapse(df, meta, by, funcs, consider_errors=True):
    what_defines_an_old_run = meta
    what_defines_a_run = [x for x in what_defines_an_old_run if x not in by]
    # group by what_defines_a_run
    if funcs is None:
        for item in by:
            assert(len(df[item].unique()) == 1)
        df = df.drop(columns=by)
    else:
        funcs["error"] = ("error", combine_errors)
        if consider_errors:
            df_err = df
        else:
            df_err = df.loc(df["error"] != "yes")
        df = df_err.groupby(what_defines_a_run).agg(**funcs).reset_index()
    return (df, what_defines_a_run)
