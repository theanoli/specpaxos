#!/bin/bash
REPO_ROOT=$(git rev-parse --show-toplevel)
CONFIG_DIR="${REPO_ROOT}/scripts"

BEEHIVE_REPO_ROOT="/home/katie/apiary/beehive"
BEEHIVE_SCRIPTS="/home/katie/apiary/beehive/util/scripts"
CORUNDUM_SCRIPTS="/home/katie/apiary/beehive/corundum_fpga/fpga/lib/pcie/scripts"

SUDO_PASSWD_FILE="${REPO_ROOT}/scripts/passwd"

declare -A FPGA_IP_PCIE=(
    ["198.0.0.9"]="03:00.0"
)
