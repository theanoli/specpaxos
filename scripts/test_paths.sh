#!/bin/bash
REPO_ROOT=$(git rev-parse --show-toplevel)
CONFIG_DIR="${REPO_ROOT}/scripts"

USERN=mgiordan

BEEHIVE_REPO_ROOT="/home/$USERN/beehive"
BEEHIVE_SCRIPTS="/home/$USERN/beehive/util/scripts"
CORUNDUM_SCRIPTS="/home/$USERN/beehive/corundum_fpga/fpga/lib/pcie/scripts"

SUDO_PASSWD_FILE="${REPO_ROOT}/scripts/passwd"

declare -A FPGA_IP_PCIE=(
    ["198.0.0.9"]="03:00.0"
)
