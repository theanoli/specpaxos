# Usage: ./run_wrapper.sh [config_path] [version_no] [num_threads] [num_reqs] [payload_size] [warmup_period]

CONFIG=${1}
VERSION_NO=${2}

DEFAULT_THREADS=1
DEFAULT_NUM_REQS=250000
DEFAULT_PAYLOAD_SIZE=256
DEFAULT_WARMUP_PERIOD=10

set -e

# iterate over threads
for threads in 1 2 4 8 16; do
	# iterate over payload size
	for payload in 32 64 128 256 512 1024; do
		./run_wrapper.sh $CONFIG $VERSION_NO $threads $DEFAULT_NUM_REQS $payload $DEFAULT_WARMUP_PERIOD
	done
done
