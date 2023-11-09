#!/bin/bash

configdir=$1   # dir with config files
cmd=$2      # command to run
logdir=$3   # log directory

passwdfile=$HOME/specpaxos/kvstore/tools/passwd

if [ "$#" -ne 3 ]; then
  echo "Usage: $0 configpath command logdir" >&2
  exit 1
fi

n=$(head -n1 $configdir/shard0.config | awk '{print $2}')  # There should at least be a shard0
let n=2*$n+1  # Count of replica machines

# SSH into each replica machine to start one process running nshard shards
for ((i=0; i<$n; i++))
do
  let line=$i+2 
  server=$(cat $config | sed -n ${line}p | awk -F'[ :]' '{print $2}')
  command="ssh $server \"mkdir -p $logdir; \
	  source $HOME/specpaxos/kvstore/tools/set_demi_env.sh; \
	  cat $passwdfile | sudo -SE nice -n -999 taskset -c 0 $cmd -c $configdir -i $i > \
	  $logdir/replica$i.log 2>&1 &\""
  echo $command
  eval $command
  echo "Done with replica $i"
done
