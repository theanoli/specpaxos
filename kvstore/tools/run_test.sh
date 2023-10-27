#!/bin/bash

if [ $# -ne 3 ]; then 
    echo "Wrong number of arguments! Need three: do_read_val nclient_machines nclient_threads"
    exit
fi

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

failure() {
	local lineno=$1
	local msg=$2
	echo "Failed at line $lineno: $msg"
}
trap 'failure ${LINENO} "$BASH_COMMAND"' ERR

# Paths to source code and logfiles.
srcdir="$HOME/specpaxos"
configdir="$srcdir/kvstore/configs/100gb_cluster"
logdir="$HOME/specpaxos/logs"
keyspath="$HOME/specpaxos/kvstore/tools/keys"

# Machines on which replicas are running.
replicas=("198.0.0.5" "198.0.0.15" "198.0.0.13")  # 100gb
# replicas=("10.100.1.16" "10.100.1.14" "10.100.1.13")
# replicas=("localhost" "localhost" "localhost")

# Machines on which clients are running.
clients=("198.0.0.1" "198.0.0.11")  # 100gb
# clients=("10.100.1.10" "10.100.1.4")
#clients=("localhost" "localhost")
# clients=("localhost")

client="benchClient"    # Which client (benchClient, retwisClient, etc)
mode="vrw"            # Mode for replicas.

validate_reads=$1
nclients=$2  # number of client machines to use
nclient_threads=$3    # number of clients to run (per machine)
nclient_procs=$((nclients * nclient_threads))
nshard=1     # number of shards
nkeys=1000 # number of keys to use
rtime=30     # duration to run

wper=50       # writes percentage
err=0        # error
zalpha=0.9    # zipf alpha (-1 to disable zipf and enable uniform)

# Print out configuration being used.
echo "Configuration:"
echo "Validate reads: $validate_reads"
echo "Shards: $nshard"
echo "Client machines: $nclients"
echo "Clients per host: $nclient_threads"
echo "Total client processes: $nclient_procs"
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
client_count=1
for host in ${clients[@]}
do
  ssh $host "mkdir -p $srcdir/logs; $srcdir/kvstore/tools/start_client.sh \"$srcdir/kvstore/$client \
  -c $configdir/shard -N $nshard -f $srcdir/kvstore/tools/keys \
  -d $rtime -w $wper -k $nkeys -m $mode -e $err -z $zalpha -h $host -r 9000\" \
  $count $nclient_threads $logdir"

  let count=$count+$nclient_threads
  client_count=$((client_count+1))
  if [ $client_count -gt $nclients ]; then 
	  break
  fi
done


# Wait for all clients to exit
echo "Waiting for client(s) to exit"
client_count=1
for host in ${clients[@]}
do
  ssh $host "$srcdir/kvstore/tools/wait_client.sh $client"
  client_count=$((client_count+1))
  if [ $client_count -gt $nclients ]; then
	  break
  fi
done


# Kill all replicas
echo "Cleaning up"
for ((i=0; i<$nshard; i++))
do
  $srcdir/kvstore/tools/stop_replica.sh $configdir/shard$i.config > /dev/null 2>&1
done


# Process logs
echo "Processing logs"
client_count=1
for host in ${clients[@]}
do
  echo "Getting log from client $host..."
  scp $host:"$logdir/client.*.log" $logdir
  client_count=$((client_count+1))
  if [ $client_count -gt $nclients ]; then
	  break
  fi
done
cat $logdir/client.*.log | sort -g -k 3 > $logdir/client.log

# Clean up logs on client and local to avoid surprises
client_count=1
for host in ${clients[@]}
do
  echo "Cleaning up log on client $host..."
  ssh $host "mkdir -p $logdir/prev_logs; mv $logdir/client.*.log $logdir/prev_logs"
  client_count=$((client_count+1))
  if [ $client_count -gt $nclients ]; then
	  break
  fi
done
rm -f $logdir/client.*.log

python3 $srcdir/kvstore/tools/process_logs.py $logdir/client.log $rtime
