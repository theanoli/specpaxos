#!/bin/bash

source ../../scripts/test_paths.sh

shard=$1    # which shard is this
config=$2   # path to config file
cmd=$3      # command to run
logdir=$4   # log directory

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 shard configpath command logdir" >&2
  exit 1
fi

n=$(head -n1 $config | awk '{print $2}')
let n=2*$n+1

for ((i=0; i<$n; i++))
do
  let line=$i+2 
  server=$(cat $config | sed -n ${line}p | awk -F'[ :]' '{print $2}')

  if [[ -n "${FPGA_IP_PCIE[${server}]}" ]]; then
    #fpga_ip_index_map["${server}"]=${REPLICA_INDEX}
    # do fpga path
    index=$i
    my_ip=$(ip route get ${server} | head -n 1 | sed 's/.*src //' | cut -d " " -f 1)
    command="${BEEHIVE_SCRIPTS}/reset_fpga_remote.sh ${SUDO_PASSWD_FILE} fpga_${index}_reset ${CORUNDUM_SCRIPTS} ${FPGA_IP_PCIE[${server}]} ${server} && sleep 5 && \
             python3 ${BEEHIVE_SCRIPTS}/beehive_vr_witness_setup.py --rep_index ${index} --witness_addr ${server} --witness_port 52001 --src_addr ${my_ip} --src_port 53212 && sleep 5"
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
    command="ssh $server \"cat ~/specpaxos/scripts/passwd | sudo -S renice -999 \\$\\$ && cat ~/specpaxos/scripts/passwd | sudo -S taskset -c 0 $cmd -c $config -i $i\" > $logdir/$shard.replica$i.log 2>&1 &"
    echo $command
    eval $command
    sleep 1
    echo "Done with replica $i"
  fi
done
