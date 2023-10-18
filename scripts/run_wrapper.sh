# 1 setup source script to set environment vars ($replicas and $logdir and what else?)
# 1 script that is user-editable, that writes all the desired configurations to run into a file
# 1 script that runs the desired configurations, but before running any, it checks to make sure it's not overwriting anything. It also puts the configurations into a timestamped log file with some output info
# 1 script that runs just 1 configuration (this)


set -e
outfile=latencies

if [ "$#" -ne "6" ]; then
	echo "not $#"
	echo "Usage: ./run_wrapper.sh [config_path] [version_no] [num_threads] [runtime] [payload_size] [warmup_period]"
	exit 2
fi

if [ -z "$BEEHIVE_PROJECT_ROOT" ]; then
	echo "source settings.sh"
	exit 1
fi

CONFIG=${1}
VERSION_NO=${2}
NUM_THREADS=${3}
RUNTIME=${4}
PAYLOAD_SIZE=${5}
WARMUP_PERIOD=${6}

logdir="logs,${CONFIG},${VERSION_NO}/"
mkdir -p "$logdir"

destfile="$NUM_THREADS,$RUNTIME,$PAYLOAD_SIZE,$WARMUP_PERIOD" # ,threadID,runNum,.raw

runNum=0
while true ; do
	if [ -e "$logdir/$destfile,0,$runNum,.raw" ]; then
		echo "Run num $runNum exists, skipping to +1"
		#exit 0
		runNum=$((runNum+1))
	else
		break
	fi
done

if [ -e "$outfile*" ]; then
	echo "ERROR: $outfile already exists. Please remove."
	exit 3
fi


# kill replicas
#for m in $replicas; do ssh $m "killall replica"; done
tn=0
./kill_replicas.sh "$CONFIG" > "$logdir/$destfile,$tn,$runNum,.stdout" 2> "$logdir/$destfile,$tn,$runNum,.stderr"

set +e
# launch replicas
./spawn_replicas.sh "$CONFIG" >> "$logdir/$destfile,$tn,$runNum,.stdout" 2>> "$logdir/$destfile,$tn,$runNum,.stderr"

sleep 3

# do the run
echo "*** *** STARTING RUNNING"
./run_simple_clients.sh "$CONFIG" $NUM_THREADS $RUNTIME $outfile $PAYLOAD_SIZE $WARMUP_PERIOD  >> "$logdir/$destfile,$tn,$runNum,.stdout" 2>> "$logdir/$destfile,$tn,$runNum,.stderr"
echo "*** *** FINISHED RUNNING"

# copy over the output files to somewhere
for tn in $(seq 0 $((NUM_THREADS-1))); do
	f=${outfile}_$tn
	mv $f "$logdir/$destfile,$tn,$runNum,.raw"
	cut -d' ' -f1 "$logdir/$destfile,$tn,$runNum,.raw" > "$logdir/$destfile,$tn,$runNum,.us"
done

# kill replicas
tn=0
./kill_replicas.sh "$CONFIG"  >> "$logdir/$destfile,$tn,$runNum,.stdout" 2>> "$logdir/$destfile,$tn,$runNum,.stderr"
