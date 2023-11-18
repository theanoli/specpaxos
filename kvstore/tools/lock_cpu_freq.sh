#!/bin/bash
set -e

# Usage: `./lock.sh <lock/restore> <CPUs>

lock() {
	CPUID=$1
	sudo cpufreq-set -c $CPUID -g userspace
	sudo cpufreq-set -c $CPUID -d 3.9GHz -u 3.9GHz
	sudo cpufreq-set -c $CPUID -f 3.9GHz
	echo 1 | sudo tee /sys/devices/system/cpu/cpu$CPUID/cpuidle/state1/disable
	echo 1 | sudo tee /sys/devices/system/cpu/cpu$CPUID/cpuidle/state2/disable
	echo 1 | sudo tee /sys/devices/system/cpu/cpu$CPUID/cpuidle/state3/disable
}

restore() {
	CPUID=$1
	echo 0 | sudo tee /sys/devices/system/cpu/cpu$CPUID/cpuidle/state3/disable
	echo 0 | sudo tee /sys/devices/system/cpu/cpu$CPUID/cpuidle/state2/disable
	echo 0 | sudo tee /sys/devices/system/cpu/cpu$CPUID/cpuidle/state1/disable
	sudo cpufreq-set -c $CPUID -f 4.0GHz
	sudo cpufreq-set -c $CPUID -d 1.0GHz -u 3.9GHz
	sudo cpufreq-set -c $CPUID -g performance

}


RUN_OPT=$1
num_cpus=$2

max_cpu=$((num_cpus-1))
cpu_list=`seq 0 $max_cpu`

if [[ "${RUN_OPT}" == "lock" ]]; then
	for i in $cpu_list; do
		lock $i
	done
else
	for i in $cpu_list; do
		restore $i
	done
fi

