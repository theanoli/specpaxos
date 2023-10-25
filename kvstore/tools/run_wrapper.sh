#!/bin/bash

set -e

CONFIG=$1
VERSION=$2

CHECK_READS=$3
CLIENT_DEVICES=$4
THREADS=$5

#og_logdir="$HOME/specpaxos/logs"

rm -rf ../../logs
mkdir -p ../../logs

./run_test.sh $CHECK_READS $CLIENT_DEVICES $THREADS

logdir=logs,$CONFIG,$VERSION

mkdir -p $logdir

destfile=$CHECK_READS,$CLIENT_DEVICES,$THREADS

runNum=0
while true ; do
	if [ -e "$logdir/$destfile,$runNum,.us" ]; then
		echo "Run num $runNum exists, skipping to +1"
		#exit 0
		runNum=$((runNum+1))
	else
		break
	fi
done

mv ../../logs/client.log $logdir/$destfile,$runNum,.us
cat ../../logs/* > $logdir/$destfile,$runNum,.stderr
