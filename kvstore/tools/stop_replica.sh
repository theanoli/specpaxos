#!/bin/bash

host=$1   # hostname (the non-CX5 interface) 

passwdfile=$HOME/specpaxos/kvstore/tools/passwd
killscript=$HOME/specpaxos/kvstore/tools/forcekill.sh

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 configpath" >&2
  exit 1
fi

command="ssh $host \"bash $killscript\""
echo $command
eval $command
