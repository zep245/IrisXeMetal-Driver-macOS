#!/bin/bash
# Installs IrisXeFB.kext (display driver) from this folder into /Library/Extensions. Restart afterwards.
# Run: bash install-fb.sh
source "$(dirname "$0")/install-kext-common.sh"
install_kext IrisXeFB.kext
