#/bin/bash

procname=$1
user=$2
check=1
while [ $check -gt 0 ]
do
    check=`pgrep -u $user -x $procname | wc -l`
    sleep 1
done
