#!/bin/bash
source test_paths.sh

# ***** the CPU map and the FPGA index map have reversed keys and values, because I want 
# to start things in a certain order
# 1. reset the FPGAs
# 2. start CPU node 0
# 3. start the rest of the CPU nodes
controlReplicas() {
    local CONFIG_FILE="${1}"
    local REPLICA_INDEX=0
    declare -A fpga_ip_index_map
    # careful this map has indexes as keys instead of IPs
    declare -A cpu_index_ip_map

    echo "reading the config file"
    # loop through the shard config file line by line, skipping the first line
    while IFS= read -r line
    do
        # Get just the IP addresses using terrifying command substitution
        local HOST_IP=$(echo "${line}" | cut -f "2" -d ' ' | cut -f "1" -d ":")
        echo "host ip is ${HOST_IP}"
        # figure out whether it's an FPGA node or a CPU node
        if [[ -n "${FPGA_IP_PCIE[${HOST_IP}]}" ]]; then
            fpga_ip_index_map["${HOST_IP}"]=${REPLICA_INDEX}
        else
            cpu_index_ip_map["${REPLICA_INDEX}"]="${HOST_IP}"
            echo "${REPLICA_INDEX}"
        fi

        REPLICA_INDEX=$((${REPLICA_INDEX}+1))
    done <<< "$(tail -n +2 ${CONFIG_FILE})"

    startFPGAs fpga_ip_index_map

    startCPUs cpu_index_ip_map
   
}

startCPUs() {
    local -n cpu_index_ip_map_ref=${1}
    echo "starting the cpu nodes"
    # copy the config into place
    for index in ${!cpu_index_ip_map_ref[@]}; do
        local cpu_ip=${cpu_index_ip_map_ref[${index}]}
        local rsync_cmd="rsync -ave 'ssh' ${CONFIG_DIR}/${CONFIG_FILE} ${cpu_ip}:${CONFIG_DIR}"
        echo ${rsync_cmd}
        eval ${rsync_cmd}
    done
    if [[ -n "${cpu_index_ip_map_ref[0]}" ]]; then
        local cpu_ip=${cpu_index_ip_map_ref[0]}
        local cpu_cmd="ssh ${cpu_ip} \"cd ${REPO_ROOT}/scripts; ${REPO_ROOT}/scripts/run_simple_replica.sh ${CONFIG_FILE} 0\""
        echo "${cpu_cmd} &"
        eval "${cpu_cmd} &"
    else
        echo "replica 0 must be a CPU node"
    fi
    sleep 3
    for index in ${!cpu_index_ip_map_ref[@]}; do
        echo "starting node ${index}"
        if [[ ${index} -eq 0 ]]; then
            continue
        fi
        sleep 3
        local cpu_ip=${cpu_index_ip_map_ref[${index}]}
        local cpu_cmd="ssh ${cpu_ip} \"cd ${REPO_ROOT}/scripts; ${REPO_ROOT}/scripts/run_simple_replica.sh ${CONFIG_FILE} ${index} \""
        echo "${cpu_cmd} &"
        eval "${cpu_cmd} &"
    done

}

startFPGAs() {
    local -n fpga_ip_index_map_ref=$1
    echo "Starting the FPGA nodes"
    # start all the FPGA nodes first
    for key in ${!fpga_ip_index_map_ref[@]}; do
        local index=${fpga_ip_index_map_ref[${key}]}
        # reset the FPGA
        local reset_cmd="${BEEHIVE_SCRIPTS}/reset_fpga_remote.sh ${SUDO_PASSWD_FILE} fpga_${index}_reset ${CORUNDUM_SCRIPTS} ${FPGA_IP_PCIE[${key}]} ${key}"
        echo ${reset_cmd}
        eval ${reset_cmd}

        # intialize the state
        # get our local ip
        local my_ip=$(ip route get ${key} | head -n 1 | sed 's/.*src //' | cut -d " " -f 1)
        local config_cmd="python3 ${BEEHIVE_SCRIPTS}/beehive_vr_witness_setup.py --rep_index ${index} --witness_addr ${key} --witness_port 51000 --src_addr ${my_ip} --src_port 53212 --config_file ${CONFIG_FILE}"
        echo ${config_cmd}
        eval ${config_cmd}
    done

}

set -e
controlReplicas ${1}
