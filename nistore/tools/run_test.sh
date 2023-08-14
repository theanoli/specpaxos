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
srcdir="/home/theano/specpaxos"
logdir="/home/theano/specpaxos/logs"

# Machines on which replicas are running.
replicas=("localhost")

# Machines on which clients are running.
clients=("localhost")

client="benchClient"    # Which client (benchClient, retwisClient, etc)
mode="vrw"            # Mode for replicas.
storemode="vrw-occ"		# Mode for storage system. 

nshard=1     # number of shards
nclient=1    # number of clients to run (per machine)
nkeys=10000 # number of keys to use
rtime=10     # duration to run

tlen=2       # transaction length
wper=0       # writes percentage
err=0        # error
skew=0       # skew
zalpha=-1    # zipf alpha (-1 to disable zipf and enable uniform)

# Print out configuration being used.
echo "Configuration:"
echo "Shards: $nshard"
echo "Clients per host: $nclient"
echo "Keys: $nkeys"
echo "Transaction Length: $tlen"
echo "Write Percentage: $wper"
echo "Error: $err"
echo "Skew: $skew"
echo "Zipf alpha: $zalpha"
echo "Skew: $skew"
echo "Client: $client"
echo "Mode: $mode"


# Generate keys to be used in the experiment.
echo "Generating random keys.."
python3 key_generator.py $nkeys > keys


# Start all replicas and timestamp servers
echo "Starting TimeStampServer replicas.."
$srcdir/nistore/tools/start_replica.sh tss $srcdir/nistore/tools/shard.tss.config \
  "$srcdir/timeserver/replica -m $mode" $logdir

for ((i=0; i<$nshard; i++))
do
  echo "Starting shard$i replicas.."
  $srcdir/nistore/tools/start_replica.sh shard$i $srcdir/nistore/tools/shard$i.config \
    "$srcdir/nistore/replica -m $storemode" $logdir
done


# Wait a bit for all replicas to start up
sleep 2


# Run the clients
echo "Running the client(s)"
count=0
for host in ${clients[@]}
do
  ssh $host "$srcdir/nistore/tools/start_client.sh \"$srcdir/nistore/$client \
  -c $srcdir/nistore/tools/shard -N $nshard -f $srcdir/nistore/tools/keys \
  -d $rtime -l $tlen -w $wper -k $nkeys -m $storemode -e $err -s $skew -z $zalpha\" \
  $count $nclient $logdir"

  let count=$count+$nclient
done


# Wait for all clients to exit
echo "Waiting for client(s) to exit"
for host in ${clients[@]}
do
  ssh $host "$srcdir/nistore/tools/wait_client.sh $client"
done


# Kill all replicas
echo "Cleaning up"
$srcdir/nistore/tools/stop_replica.sh $srcdir/nistore/tools/shard.tss.config > /dev/null 2>&1
for ((i=0; i<$nshard; i++))
do
  $srcdir/nistore/tools/stop_replica.sh $srcdir/nistore/tools/shard$i.config > /dev/null 2>&1
done


# Process logs
echo "Processing logs"
cat $logdir/client.*.log | sort -g -k 3 > $logdir/client.log
rm -f $logdir/client.*.log

python3 $srcdir/nistore/tools/process_logs.py $logdir/client.log $rtime
