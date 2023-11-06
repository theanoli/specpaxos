#!/bin/bash

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 command begin_id ncopies logdir" >&2
  exit 1
fi

cmd=$1
begin=$2
copies=$3
logdir=$4

let end=$begin+$copies

source $HOME/specpaxos/kvstore/tools/set_demi_env.sh

counter=0
for ((i=$begin; i<$end; i++))
do
  cmd="$cmd -i $counter"
  cpuid=$((counter % `nproc`))
  if [ $begin == 0 ]; then
	  command="echo splashed-grimace-grass3 | sudo -SE renice -999 \$BASHPID && taskset -c $cpuid $cmd -p > $logdir/client.$i.log 2>&1 &"
  else 
	  command="echo splashed-grimace-grass3 | sudo -SE renice -999 \$BASHPID && taskset -c $cpuid $cmd > $logdir/client.$i.log 2>&1 &"
  fi 
  echo $command
  eval $command
  counter=$((counter+1))
done
