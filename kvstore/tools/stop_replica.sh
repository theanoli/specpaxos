#!/bin/bash

config=$1   # path to config file

passwdfile=$HOME/specpaxos/kvstore/tools/passwd
killscript=$HOME/specpaxos/kvstore/tools/forcekill.sh

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 configpath" >&2
  exit 1
fi

n=$(head -n1 $config | awk '{print $2}')
let n=2*$n+1

for ((i=0; i<$n; i++))
do
  let line=$i+2 
  server=$(cat $config | sed -n ${line}p | awk -F'[ :]' '{print $2}')
  command="ssh $server \"bash $killscript\""
  echo $command
  eval $command
done
