#!/bin/bash
set -e 
if [ $# -ne 2 ]; then 
    echo "Wrong number of arguments! Need <run> or <collect>, then VERSION"
    exit
fi

VERSION=$2

REPO_ROOT=$(git rev-parse --show-toplevel)
CONFIG_DIRS=(
   100gb_cluster
    fpga_cluster
)

NUM_CLIENT_MACHINES=(
    #1
    2
)

NUM_MACHINE_THREADS=(
    #1
    #2
    #4
    #6
    #8
    #10
    #12
    #16
    #24
    #32
    #64
    #96
    #128
    192
    #256
    #512
)

NUM_SHARDS=(
    #1
    #2
    3
    #4
)

SCRIPTS_PATH="${REPO_ROOT}/kvstore/tools"
RESULT_DIR="${REPO_ROOT}/kvstore/results/${VERSION}"
CONFIG_PATH="${REPO_ROOT}/kvstore/configs"
COLLECTED_RESULTS_FILE="${RESULT_DIR}/collected_results.csv"

RUNTIME=30
MEASURE_ENERGY=true

runBenchmark() {
    mkdir -p ${RESULT_DIR}

    echo "Enabling IRQs..."
    _cpuconfigdir=../configs/100gb_cluster
    _replicas=$(cat "$_cpuconfigdir"/shard0.config | tail -n+2 | cut -d' ' -f2 | cut -d':' -f1)
    for replica in $_replicas; do
        ssh $replica "cd `pwd`; cat ../../scripts/passwd | sudo -S ./set_irqs.sh "'`ifconfig | grep 198.0.0. -B1 | head -n1 | cut -d: -f1` 1'
    done

    echo "Locking CPU Frequencies..."
    ./lock_all.sh lock

    echo "Disabling turbo..."
    for replica in $_replicas; do
        ssh $replica "cd `pwd`; cat ../../scripts/passwd | sudo -S ./turbo_boosting.sh disable"
    done

    echo "Sleeping..."
    sleep 5 # wait for CPUs to cool down


    for CONFIG in "${CONFIG_DIRS[@]}"; do
        for SHARDS in "${NUM_SHARDS[@]}"; do
            for CLIENT_MACHINES in "${NUM_CLIENT_MACHINES[@]}"; do
                for MACHINE_THREADS in "${NUM_MACHINE_THREADS[@]}"; do
                    energy_files=0
                    while [ $energy_files -ne 3 ]; do
                        echo "Running ${CONFIG} with ${SHARDS} shards on ${CLIENT_MACHINES} client nodes with ${MACHINE_THREADS} per node"
                        RUN_DIR="${RESULT_DIR}/${CONFIG}_${SHARDS}shard_${CLIENT_MACHINES}clientnodes_${MACHINE_THREADS}threads"
                        mkdir -p ${RUN_DIR}
                        CMD="${SCRIPTS_PATH}/run_test.sh ${CONFIG_PATH}/${CONFIG} true ${CLIENT_MACHINES} ${MACHINE_THREADS} ${SHARDS} ${RUN_DIR} $MEASURE_ENERGY $RUNTIME >> ${RUN_DIR}/run.out 2>&1"
                        echo "${CMD}" > "${RUN_DIR}/run.out"
                        eval "${CMD}"

                        if "$MEASURE_ENERGY"; then
                            energy_files=`find ${RUN_DIR} | grep energy | wc -l`
                            echo "Found ${energy_files} energy files" >> ${RUN_DIR}/run.out
                        else
                            energy_files=3
                        fi
                    done

                    if "$MEASURE_ENERGY"; then
                        ENERGY_IP=""
                        if [[ "${CONFIG}" == "100gb_cluster" ]]; then
                            ENERGY_IP="198.0.0.15"
                        else
                            ENERGY_IP="198.0.0.9"
                        fi

                        python3 ${REPO_ROOT}/kvstore/tools/process_logs.py --latencies ${RUN_DIR}/client.log --runtime ${RUNTIME} --energies ${RUN_DIR}/energy_${ENERGY_IP}.out >> "${RUN_DIR}/run.out"
                    fi
                done
            done
        done
    done

    echo "Disabling IRQs..."
    for replica in $_replicas; do
        ssh $replica "cd `pwd`; cat ../../scripts/passwd | sudo -S ./set_irqs.sh "'`ifconfig | grep 198.0.0. -B1 | head -n1 | cut -d: -f1` 2'
    done
    echo "Unlocking CPU Frequencies..."
    ./lock_all.sh restore
    echo "Enabling turbo..."
    for replica in $_replicas; do
        ssh $replica "cd `pwd`; cat ../../scripts/passwd | sudo -S ./turbo_boosting.sh enable"
    done
}

collectResults() {
    echo "config,num_shards,num_client_nodes,num_node_threads,num_txns,thruput,lat_avg,lat_med,lat_99,energy_j" > ${COLLECTED_RESULTS_FILE}
    for CONFIG in "${CONFIG_DIRS[@]}"; do
        for SHARDS in "${NUM_SHARDS[@]}"; do
            for CLIENT_MACHINES in "${NUM_CLIENT_MACHINES[@]}"; do
                for MACHINE_THREADS in "${NUM_MACHINE_THREADS[@]}"; do
                    RUN_DIR="${RESULT_DIR}/${CONFIG}_${SHARDS}shard_${CLIENT_MACHINES}clientnodes_${MACHINE_THREADS}threads"
                    RES_FILE="${RUN_DIR}/run.out"
                    echo -n "${CONFIG},${SHARDS},${CLIENT_MACHINES},${MACHINE_THREADS}," >> ${COLLECTED_RESULTS_FILE}
                    tail -n 12 ${RES_FILE} | sed -n "1p;3p;7p;8p;9p;12p" | grep -o "\S*$" | tr '\n' ',' | sed 's/.$//' >> ${COLLECTED_RESULTS_FILE}
                    echo "" >> ${COLLECTED_RESULTS_FILE}
                done
            done
        done
    done
}

RUN_OPT=$1

if [[ "${RUN_OPT}" == "run" ]]; then
    runBenchmark
else
    collectResults
fi


