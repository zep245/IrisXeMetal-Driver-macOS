#!/bin/bash
# IrisXe v0.68.45: installs everything in one go - IrisXeCore into the OpenCore EFI, IrisXeFB / IrisXeAccel / IrisXeAGDC
# into /Library/Extensions (one kext cache rebuild), IrisXeMTLDriver.bundle into /Library/GPUBundles.
# Restart afterwards (after allowing the kexts in Privacy & Security if macOS asks).
# Run: bash install-all.sh
cd "$(dirname "$0")" || exit 1
sudo -v || exit 1
echo "==== 1/4 IrisXeCore (OpenCore EFI)"
bash install-core.sh || { echo "STOPPED: IrisXeCore was not installed (see above). Nothing else was changed."; exit 1; }
echo "==== 2/4 IrisXeFB, IrisXeAccel, IrisXeAGDC (/Library/Extensions)"
source ./install-kext-common.sh
export IRISXE_NO_CACHE=1
for K in IrisXeFB.kext IrisXeAccel.kext IrisXeAGDC.kext; do
    ( install_kext "$K" ) || { echo "STOPPED at $K"; exit 1; }
done
unset IRISXE_NO_CACHE
echo "==== 3/4 rebuilding the kext cache (can take a minute)"
irisxe_kmutil
echo "==== 4/4 IrisXeMTLDriver.bundle (/Library/GPUBundles)"
bash install-bundle.sh || { echo "STOPPED: the bundle was not installed"; exit 1; }
echo
echo "All installed."
irisxe_after_note
echo "After the restart the GPU draws the desktop from boot (boot-arg -irisxecpu turns it off)."
