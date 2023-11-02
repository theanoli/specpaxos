#!/bin/bash
source test_paths.sh
CLIENT_BINARY="${REPO_ROOT}/bench/client"
CONFIG_PATH="${CONFIG_DIR}/simple_config.txt"

if [ "$#" -ne 6 ]; then
    echo "Usage: $0 [config_path] [num_threads] [runtime] [req_latencies_file] [payload_size] [warmup_period]"
    exit 1
fi
set -e

runClients() {
    local config_path=${1}
    local num_threads=${2}
    local runtime=${3}
    local req_latencies_file=${4}
    local payload_size=${5}
    local warmup_period=${6}

    local CMD="${CLIENT_BINARY} -c ${config_path} -m vrw -a -n ${runtime} -t ${NUM_THREADS} -f ${req_latencies_file} -s ${payload_size} -w ${warmup_period}"
    echo ${CMD}
    cat ~/specpaxos/scripts/passwd | sudo -S renice -999 $$
    eval ${CMD}
}

CONFIG=${1}
NUM_THREADS=${2}
RUNTIME=${3}
LATENCIES=${4}
PAYLOAD_SIZE=${5}
WARMUP_PERIOD=${6}

runClients ${CONFIG} ${NUM_THREADS} ${RUNTIME} ${LATENCIES} ${PAYLOAD_SIZE} ${WARMUP_PERIOD}
