#!/bin/bash
if [ $# -ne 1 ]; then 
    echo "Usage: set_irqs <devname>"
fi

DEV_NAME=$1

IRQS=$(cat /proc/interrupts | grep ${DEV_NAME} | cut -d ":" -f1)

bitshift=0
lower=1
for IRQ in ${IRQS[@]}; do
    irq_mask=$((1 << $bitshift))
    bitmask=$(printf "%08x" ${irq_mask})
   
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
