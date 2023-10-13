#!/bin/bash

trap '{
  echo "\nKilling all clients.. Please wait..";
  for host in ${clients[@]}
  do
    ssh $host "killall -9 $client";
    ssh $host "killall -9 $client";
  done

  echo "\nKilling all replics.. Please wait..";
  for host in ${servers[@]}
  do
    ssh $host "killall -9 server";
  done
}' INT

# Paths to source code and logfiles.
srcdir="$HOME/specpaxos"
configdir="$srcdir/kvstore/configs/local"
logdir="$HOME/specpaxos/logs"
keyspath="$HOME/specpaxos/kvstore/tools/keys"

# Machines on which replicas are running.
# replicas=("198.0.0.5" "198.0.0.15" "198.0.0.13")
# replicas=("10.100.1.16" "10.100.1.14" "10.100.1.13")
replicas=("localhost" "localhost" "localhost")

# Machines on which clients are running.
# clients=("198.0.0.1" "198.0.0.11") #"10.100.1.4")
# clients=("10.100.1.10" "10.100.1.4")
#clients=("localhost" "localhost")
clients=("localhost")

client="benchClient"    # Which client (benchClient, retwisClient, etc)
mode="vrw"            # Mode for replicas.

validate_reads=$1
nshard=1     # number of shards
nclient=3    # number of clients to run (per machine)
nkeys=1000 # number of keys to use
rtime=60     # duration to run

wper=1       # writes percentage
err=0        # error
zalpha=0.9    # zipf alpha (-1 to disable zipf and enable uniform)

# Print out configuration being used.
echo "Configuration:"
echo "Validate reads: $validate_reads"
echo "Shards: $nshard"
echo "Clients per host: $nclient"
echo "Keys: $nkeys"
echo "Write Percentage: $wper"
echo "Error: $err"
echo "Zipf alpha: $zalpha"
echo "Client: $client"
echo "Mode: $mode"

# Distribute the config files to the client and host machines
for host in ${clients[@]}
do
  scp $configdir/*.config $host:$configdir
done
for host in ${replicas[@]}
do
  scp $configdir/*.config $host:$configdir
done

# Generate keys to be used in the experiment.
echo "Generating random keys..."
python3 $srcdir/kvstore/tools/key_generator.py $nkeys > $keyspath
for host in ${clients[@]}
do
  scp $keyspath $host:$keyspath
done


replica_cmd="$srcdir/kvstore/replica -m $mode"
if "$validate_reads"; then 
	replica_cmd="$replica_cmd -s"
fi
for ((i=0; i<$nshard; i++))
do
  echo "Starting replicas for $i shards..."
  $srcdir/kvstore/tools/start_replica.sh shard$i $configdir/shard$i.config \
    "$replica_cmd" $logdir
done


# Wait a bit for all replicas to start up
sleep 2


# Run the clients
echo "Running the client(s)"
count=0
for host in ${clients[@]}
do
  ssh $host "mkdir -p $srcdir/logs; $srcdir/kvstore/tools/start_client.sh \"$srcdir/kvstore/$client \
  -c $configdir/shard -N $nshard -f $srcdir/kvstore/tools/keys \
  -d $rtime -w $wper -k $nkeys -m $mode -e $err -z $zalpha\" \
  $count $nclient $logdir"

  let count=$count+$nclient
done


# Wait for all clients to exit
echo "Waiting for client(s) to exit"
for host in ${clients[@]}
do
  ssh $host "$srcdir/kvstore/tools/wait_client.sh $client"
done


# Kill all replicas
echo "Cleaning up"
for ((i=0; i<$nshard; i++))
do
  $srcdir/kvstore/tools/stop_replica.sh $configdir/shard$i.config > /dev/null 2>&1
done


# Process logs
echo "Processing logs"
for host in ${clients[@]}
do
  scp $host:"$logdir/client.*.log" $logdir
done
cat $logdir/client.*.log | sort -g -k 3 > $logdir/client.log
rm -f $logdir/client.*.log

python3 $srcdir/kvstore/tools/process_logs.py $logdir/client.log $rtime
