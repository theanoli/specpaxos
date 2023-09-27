#!/bin/bash
source test_paths.sh
CLIENT_BINARY="${REPO_ROOT}/bench/client"
CONFIG_PATH="${CONFIG_DIR}/simple_config.txt"

if [ "$#" -ne 5 ]; then
    echo "Usage: $0 [config_path] [num_threads] [num_reqs] [req_latencies_file] [payload_size]"
    exit 1
fi
set -e

runClients() {
    local config_path=${1}
    local num_threads=${2}
    local num_reqs=${3}
    local req_latencies_file=${4}
    local payload_size=${5}

    local CMD="${CLIENT_BINARY} -c ${config_path} -m vrw -n ${num_reqs} -t ${NUM_THREADS} -f ${req_latencies_file} -s ${payload_size}"
    echo ${CMD}
    eval ${CMD}
}

CONFIG=${1}
NUM_THREADS=${2}
NUM_REQS=${3}
LATENCIES=${4}
PAYLOAD_SIZE=${5}

runClients ${CONFIG} ${NUM_THREADS} ${NUM_REQS} ${LATENCIES} ${PAYLOAD_SIZE}
