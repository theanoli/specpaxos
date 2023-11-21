#!/bin/bash
#set -e

LOCK=$1 # either lock or restore

SHARDS_MAX=4
CLIENT_CORES=32

configdir=../configs/100gb_cluster
replicas=$(cat "$configdir"/shard0.config | tail -n+2 | cut -d' ' -f2 | cut -d':' -f1)
clients="198.0.0.1 198.0.0.11"

#for client in $clients; do
	#echo "Client: $client"
	#ssh $client "cd $(pwd); cat ../../scripts/passwd | sudo -S ./lock_cpu_freq.sh $LOCK $CLIENT_CORES"
#done
for replica in $replicas; do
	echo "Replica: $replica"
	ssh $replica "cd $(pwd); cat ../../scripts/passwd | sudo -S ./lock_cpu_freq.sh $LOCK $SHARDS_MAX"
done
