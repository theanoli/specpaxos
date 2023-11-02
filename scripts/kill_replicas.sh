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
    #declare -A fpga_ip_index_map
    # careful this map has indexes as keys instead of IPs
    #declare -A cpu_index_ip_map

    echo "reading the config file"
    # loop through the shard config file line by line, skipping the first line
    while IFS= read -r line
    do
        # Get just the IP addresses using terrifying command substitution
        local HOST_IP=$(echo "${line}" | cut -f "2" -d ' ' | cut -f "1" -d ":")
        echo "host ip is ${HOST_IP}"
        # figure out whether it's an FPGA node or a CPU node
        if [[ -n "${FPGA_IP_PCIE[${HOST_IP}]}" ]]; then
            #fpga_ip_index_map["${HOST_IP}"]=${REPLICA_INDEX}
	    echo "skipping ${HOST_IP}"
        else
            #cpu_index_ip_map["${REPLICA_INDEX}"]="${HOST_IP}"
	    #set +e
	    ssh -n ${HOST_IP} "cat ~/specpaxos/scripts/passwd | sudo -S killall replica"
	    #set -e
	    echo "killed ${HOST_IP}"
        fi

        #REPLICA_INDEX=$((${REPLICA_INDEX}+1))
    done <<< "$(tail -n +2 ${CONFIG_FILE})"

}

#set -e
controlReplicas ${1}
