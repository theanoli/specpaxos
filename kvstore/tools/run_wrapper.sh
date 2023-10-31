#!/bin/bash

set -e

CONFIG=$1
VERSION=$2

CHECK_READS=$3
CLIENT_DEVICES=$4
THREADS=$5

config_base=$(basename "$CONFIG")

#og_logdir="$HOME/specpaxos/logs"

rm -rf ../../logs
mkdir -p ../../logs

logdir=logs,$config_base,$VERSION

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

date > $logdir/$destfile,$runNum,.stdout

./run_test.sh $CONFIG $CHECK_READS $CLIENT_DEVICES $THREADS

mv ../../logs/client.log $logdir/$destfile,$runNum,.us
cat ../../logs/* > $logdir/$destfile,$runNum,.stderr
