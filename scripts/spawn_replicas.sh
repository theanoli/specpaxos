#!/bin/bash
source test_paths.sh
source "${BEEHIVE_SCRIPTS}/reset_fpga_remote.sh"
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 [replica_cmd: spawn kill]"
fi

REPLICA_CMD=$1

controlReplicas() {
    local INPUT_FILE="${REPLICA_CONFIG_FILE}"
    local REPLICA_INDEX=0
    declare -A fpga_ip_index_map
    declare -A cpu_ip_index_map

    # loop through the shard config file line by line, skipping the first line
    while IFS= read -r line
    do
        # Get just the IP addresses using terrifying command substitution
        local HOST_IP=$(echo "${line}" | cut -f "2" -d ' ' | cut -f "1" -d ":")
        # figure out whether it's an FPGA node or a CPU node
        if [[ -n "${FPGA_IP_PCIE[${HOST_IP}]}" ]]; then
            fpga_ip_index_map["${HOST_IP}"]=${REPLICA_INDEX}
        else
            cpu_ip_index_map["${HOST_IP}"]=${REPLICA_INDEX}
        fi

        REPLICA_INDEX=$((${REPLICA_INDEX}+1))
    done <<< "$(tail -n +2 ${INPUT_FILE})"

    # start all the FPGA nodes first
    for key in ${!fpga_ip_index_map[@]}; do
        local index=${fpga_ip_index_map[${key}]}
        # reset the FPGA
        local reset_cmd="start_fpga_server ${SUDO_PASSWD_FILE} fpga_${index}_reset ${BEEHIVE_SCRIPTS} ${FPGA_IP_PCIE[${key}]} ${key}"
        echo ${reset_cmd}
        eval ${reset_cmd}

        # intialize the state
        # get our local ip
        local my_ip=$(ip route get ${key} | head -n 1 | sed 's/.*src //' | cut -d " " -f 1)
        local config_cmd="python3 ${BEEHIVE_SCRIPTS}/beehive_vr_witness_setup.py --rep_index ${index} --witness_addr ${key} --witness_port 52001 --src_addr ${my_ip} --src_port 53212"
        echo ${config_cmd}
        eval ${reset_cmd}
    done
    
    for key in ${!cpu_ip_index_map[@]}; do
        sleep 3
        local cpu_index=${cpu_ip_index_map[${key}]}
        local cpu_cmd="ssh ${key} \"${REPO_ROOT}/scripts/run_simple_replica.sh ${cpu_index} simple_config\""
    done
}

set -e
controlReplicas
