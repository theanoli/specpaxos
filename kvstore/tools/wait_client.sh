#/bin/bash

procname=$1
user=$2
check=1
while [ $check -gt 0 ]
do
    echo Checking for $procname...
    check=`pgrep -x $procname | wc -l`
    sleep 3
done
