#!/bin/bash
source test_paths.sh
SERVER_BINARY="${REPO_ROOT}/bench/replica"

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 [replica index] [config file]"
    exit 1
fi
set -e

CONFIG_PATH="${1}"
REPLICA_INDEX=${2}

runReplica() {
    local config_file=${1}
    local replica_index=${2}

    local config="${CONFIG_DIR}/${config_file}"

    local cmd="cat ~/specpaxos/scripts/passwd | sudo -S taskset -c 0 ${SERVER_BINARY} -c ${config} -m vrw -i ${replica_index}"
    echo "${cmd}"
    eval "${cmd}"
}

runReplica ${CONFIG_PATH} ${REPLICA_INDEX}
