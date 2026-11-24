#!/bin/bash
# Installs IrisXeMTLDriver.bundle (Metal driver) from this folder into /Library/GPUBundles.
# No restart needed: apps started afterwards use it (restart WindowServer or log out for the desktop).
# Run: bash install-bundle.sh
cd "$(dirname "$0")" || exit 1
B=IrisXeMTLDriver.bundle
if [ ! -d "$B" ]; then echo "ERROR: $B is not in $(pwd)"; exit 1; fi
V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$B/Contents/Info.plist" 2>/dev/null || echo "?")
OLD=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "/Library/GPUBundles/$B/Contents/Info.plist" 2>/dev/null || echo "none")
echo "== $B: installed $OLD -> new $V"
sudo mkdir -p /Library/GPUBundles
sudo rm -rf "/Library/GPUBundles/$B" || exit 1
sudo cp -R "$B" /Library/GPUBundles/ || exit 1
sudo chown -R root:wheel "/Library/GPUBundles/$B"
sudo xattr -cr "/Library/GPUBundles/$B"
echo "== done: $B $(defaults read "/Library/GPUBundles/$B/Contents/Info" CFBundleShortVersionString 2>/dev/null) is installed."
