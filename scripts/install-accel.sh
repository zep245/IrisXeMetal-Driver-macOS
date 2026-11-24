#!/bin/bash
# Installs IrisXeAccel.kext (Metal accelerator) from this folder into /Library/Extensions. Restart afterwards.
# Run: bash install-accel.sh
source "$(dirname "$0")/install-kext-common.sh"
install_kext IrisXeAccel.kext
