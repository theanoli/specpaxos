#!/bin/bash
if [ $# -ne 2 ]; then 
    echo "Usage: set_irqs <devname> <num_numa_nodes>"
fi

DEV_NAME=$1
NUM_NUMA_NODES=$2

IRQS=$(cat /proc/interrupts | grep ${DEV_NAME} | cut -d ":" -f1)

bitshift=0
lower=1
for IRQ in ${IRQS[@]}; do
    irq_mask=$((1 << $bitshift))

    bitmask=""
    if [ ${NUM_NUMA_NODES} -eq 1 ]; then
        bitmask=$(printf "%08x" ${irq_mask})
    elif [ ${NUM_NUMA_NODES} -eq 2 ]; then
        if [ ${lower} -eq 1 ]; then
            bitmask=$(printf "%04x0000" ${irq_mask})
        else
            bitmask=$(printf "%08x" ${irq_mask})
        fi
    else
        echo "Unsupported number of NUMA nodes ${NUM_NUMA_NODES}"
        exit 1
    fi
   
    cpu_mask=""
    if [ ${lower} -eq 1 ]; then
        cpu_mask="00000000,${bitmask}"
    else
        cpu_mask="${bitmask},00000000"
    fi

    echo ${cpu_mask} | sudo tee "/proc/irq/${IRQ}/smp_affinity"

    if [ ${bitshift} -eq 15 ]; then
        lower=$((!lower))
    fi
    bitshift=$(((bitshift+1)%16))
done
