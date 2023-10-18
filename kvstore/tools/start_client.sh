#!/bin/bash

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 command begin_id ncopies" >&2
  exit 1
fi

cmd=$1
begin=$2
copies=$3
logdir=$4

let end=$begin+$copies

counter=0
for ((i=$begin; i<$end; i++))
do
  cpuid=$((counter % `nproc`))
  if [ $begin == 0 ]; then
	  command="taskset -c $cpuid $cmd -p > $logdir/client.$i.log 2>&1 &"
  else 
	  command="taskset -c $cpuid $cmd > $logdir/client.$i.log 2>&1 &"
  fi 
  echo $command
  eval $command
  counter=$((counter+1))
done
