#!/bin/bash
# Installs IrisXeCore.kext into the OpenCore EFI (EFI/OC/Kexts), where OpenCore injects it at boot.
# Looks at every EFI partition and uses the one that already has EFI/OC/Kexts/IrisXeCore.kext; stops if it finds
# none or more than one. The old kext is kept next to it as IrisXeCore.kext.old (delete it later if all is well).
# Restart afterwards. Run: bash install-core.sh
cd "$(dirname "$0")" || exit 1
K=IrisXeCore.kext
if [ ! -d "$K" ]; then echo "ERROR: $K is not in $(pwd)"; exit 1; fi
# v0.58.3: never put an incomplete kext into the EFI (on 21 Nov an empty IrisXeCore.kext ended up there and the GPU
# driver did not load)
if [ ! -f "$K/Contents/Info.plist" ] || [ ! -s "$K/Contents/MacOS/IrisXeCore" ]; then
    echo "ERROR: $K in $(pwd) is incomplete (no Contents/Info.plist or Contents/MacOS/IrisXeCore). Download the build again."
    exit 1
fi
V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$K/Contents/Info.plist" 2>/dev/null || echo "?")
found=()
for p in $(diskutil list | awk '/EFI/ {print $NF}'); do
    sudo diskutil mount "$p" >/dev/null 2>&1
    mp=$(diskutil info "$p" | awk -F': *' '/Mount Point/ {print $2}')
    [ -n "$mp" ] && [ -d "$mp/EFI/OC/Kexts/$K" ] && found+=("$mp")
done
if [ "${#found[@]}" -ne 1 ]; then
    echo "ERROR: expected exactly one EFI partition with EFI/OC/Kexts/$K, found ${#found[@]}: ${found[*]}"
    echo "Nothing was changed."
    exit 1
fi
D="${found[0]}/EFI/OC/Kexts"
OLD=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$D/$K/Contents/Info.plist" 2>/dev/null || echo "?")
echo "== $K in $D: installed $OLD -> new $V"
sudo rm -rf "$D/$K.old"
sudo mv "$D/$K" "$D/$K.old" || exit 1
sudo cp -R "$K" "$D/" || { echo "copy failed, putting the old one back"; sudo mv "$D/$K.old" "$D/$K"; exit 1; }
sudo xattr -cr "$D/$K"
if ! cmp -s "$K/Contents/MacOS/IrisXeCore" "$D/$K/Contents/MacOS/IrisXeCore" || ! cmp -s "$K/Contents/Info.plist" "$D/$K/Contents/Info.plist"; then
    echo "ERROR: the copy in the EFI is not the same as the new kext (EFI full?). Putting the old one back."
    df -h "${found[0]}"
    sudo rm -rf "$D/$K"; sudo mv "$D/$K.old" "$D/$K"
    exit 1
fi
echo "== done: $K $V is in the EFI (old one kept as $K.old). Restart the Mac to load it."
