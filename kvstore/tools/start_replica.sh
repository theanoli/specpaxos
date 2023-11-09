#!/bin/bash

source ../../scripts/test_paths.sh

shard_num=$1    # which shard is this
config=$2   # path to config file
cmd=$3      # command to run
logdir=$4   # log directory
reset_fpga=$5

set -e

if [ "$#" -ne 5 ]; then
  echo "Usage: $0 shard configpath command logdir reset_fpga" >&2
  exit 1
fi

n=$(head -n1 $config | awk '{print $2}')
let n=2*$n+1

for ((i=0; i<$n; i++))
do
  let line=$i+2 
  server=$(cat $config | sed -n ${line}p | awk -F'[ :]' '{print $2}')
  server_port=$(cat $config | sed -n ${line}p | awk -F'[ :]' '{print $3}')

  if [[ -n "${FPGA_IP_PCIE[${server}]}" ]]; then
    #fpga_ip_index_map["${server}"]=${REPLICA_INDEX}
    # do fpga path
    index=$i
    my_ip=$(ip route get ${server} | head -n 1 | sed 's/.*src //' | cut -d " " -f 1)
    command=""
    if [[ ${reset_fpga} != 0 ]]; then
        command="${BEEHIVE_SCRIPTS}/reset_fpga_remote.sh ${SUDO_PASSWD_FILE} fpga_${index}_reset ${CORUNDUM_SCRIPTS} ${FPGA_IP_PCIE[${server}]} ${server} && "
    fi
    command="${command} python3 ${BEEHIVE_SCRIPTS}/beehive_vr_witness_setup.py --rep_index ${index} --witness_addr ${server} --witness_port ${server_port} --src_addr ${my_ip} --src_port 53212 --config_file ${config}"
    echo $command
    eval $command
    echo "Done with replica $i"
  fi
done

for ((i=0; i<$n; i++))
do
  let line=$i+2 
  server=$(cat $config | sed -n ${line}p | awk -F'[ :]' '{print $2}')

  if [[ -n "${FPGA_IP_PCIE[${server}]}" ]]; then
    true
  else
    command="ssh $server \"cat ${SUDO_PASSWD_FILE} | sudo -S renice -999 \\$\\$ && cat ${SUDO_PASSWD_FILE} | sudo -S taskset -c $shard_num $cmd -c $config -i $i\" > $logdir/shard$shard_num.replica$i.log 2>&1 &"
    echo $command
    eval $command
    sleep 1
    echo "Done with replica $i"
  fi
done
