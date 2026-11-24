#!/bin/bash
# Installs IrisXeAGDC.kext into /Library/Extensions. Restart afterwards.
# v0.64.0: it loads by itself at boot and creates the AGDC for the GPU-drawn desktop (IrisXeAccel asks for it).
# Run: bash install-agdc.sh
source "$(dirname "$0")/install-kext-common.sh"
install_kext IrisXeAGDC.kext
