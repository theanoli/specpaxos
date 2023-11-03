#!/bin/bash -x

set -e

for i in 1 2 3 4; do
	./run_wrapper.sh $1 $2 false 1 1
	./run_wrapper.sh $1 $2 false 1 2
	./run_wrapper.sh $1 $2 false 1 4
	./run_wrapper.sh $1 $2 false 1 8
	./run_wrapper.sh $1 $2 false 1 16
	./run_wrapper.sh $1 $2 false 1 32
done
