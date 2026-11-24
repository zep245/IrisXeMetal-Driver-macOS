#!/bin/bash
# Shared part of install-accel.sh / install-fb.sh: copies one kext from this folder into /Library/Extensions
# and rebuilds the kext cache. Usage (from the other scripts): install_kext IrisXeAccel.kext
install_kext() {
    local K="$1"
    cd "$(dirname "$0")" || exit 1
    if [ ! -d "$K" ]; then echo "ERROR: $K is not in $(pwd)"; exit 1; fi
    local V
    V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$K/Contents/Info.plist" 2>/dev/null || echo "?")
    local OLD
    OLD=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "/Library/Extensions/$K/Contents/Info.plist" 2>/dev/null || echo "none")
    echo "== $K: installed $OLD -> new $V"
    sudo rm -rf "/Library/Extensions/$K" || exit 1
    sudo cp -R "$K" /Library/Extensions/ || exit 1
    sudo chown -R root:wheel "/Library/Extensions/$K"
    sudo chmod -R 755 "/Library/Extensions/$K"
    sudo xattr -cr "/Library/Extensions/$K"
    if [ -n "$IRISXE_NO_CACHE" ]; then echo "== $K $V copied"; return 0; fi   # v0.64.0: install-all.sh rebuilds once
    echo "== rebuilding the kext cache (can take a minute)"
    irisxe_kmutil
    echo "== done: $K $V is installed."
    irisxe_after_note
}

# v0.64.1: on macOS 13+ kmutil also tries Apple's own caches and prints "Missing Developer Kit" for them. That part
# is expected: the cache of /Library/Extensions is still updated (23 Nov: "AuxKC bundle ... marked as loadable").
irisxe_kmutil() {
    sudo kmutil install --update-all 2>&1 | grep -vE "Missing Developer Kit|KMErrorDomain|extension manager, expect errors|UserInfo=|^$" | grep -v "^checking collections"
    return 0
}
irisxe_after_note() {
    echo
    echo "NEXT: open System Settings > Privacy & Security, scroll down. If it says system software was blocked or"
    echo "      shows an Allow button: click Allow (password), then restart when it asks. Otherwise restart now."
}
