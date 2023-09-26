#!/bin/bash
REPO_ROOT=$(git rev-parse --show-toplevel)
CONFIG_DIR="${REPO_ROOT}/scripts"

BEEHIVE_REPO_ROOT="/home/katie/apiary/beehive"
BEEHIVE_SCRIPTS="/home/katie/apiary/beehive/util/scripts"
 python3 beehive_vr_witness_setup.py --rep_index 1 --witness_addr 198.0.0.9 --witness_port 52001 --src_addr 198.0.0.11 --src_port 53212

SUDO_PASSWD_FILE="${REPO_ROOT}/scripts/passwd"

declare -A FPGA_IP_PCIE=(
    ["198.0.0.9"]="03:00.0"
)
