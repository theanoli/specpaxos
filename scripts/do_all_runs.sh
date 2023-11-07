# Usage: ./run_wrapper.sh [config_path] [version_no] [num_threads] [runtime] [payload_size] [warmup_period]

CONFIG=${1}
VERSION_NO=${2}

DEFAULT_THREADS=1
#DEFAULT_NUM_REQS=300000
DEFAULT_RUNTIME=30000000000
DEFAULT_PAYLOAD_SIZE=256
DEFAULT_WARMUP_PERIOD=10

set -e

# iterate over threads
for threads in 1 2 4 8 16 32; do
	# iterate over payload size
	for payload in 32 64 128 256 512 1024; do
		./run_wrapper.sh $CONFIG $VERSION_NO $threads $DEFAULT_RUNTIME $payload $DEFAULT_WARMUP_PERIOD
	done
done
