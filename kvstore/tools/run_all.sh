#!/bin/bash

set -e

./run_wrapper.sh $1 $2 false 1 1
./run_wrapper.sh $1 $2 false 1 2
./run_wrapper.sh $1 $2 false 1 4
./run_wrapper.sh $1 $2 false 1 8
./run_wrapper.sh $1 $2 false 1 16
./run_wrapper.sh $1 $2 false 1 32
./run_wrapper.sh $1 $2 false 1 64
