#!/bin/bash
# IrisXe v0.66.1 wallpaper test: do Sequoia's live wallpapers work with our Metal device?
# Sequoia's wallpapers are drawn by WallpaperSequoiaExtension with Metal. Until v0.64 our Metal device existed only in
# WindowServer, so the renderer failed with "SequoiaRenderer.Error (0)". v0.65.0 gives it our device (app list).
# No restart needed: only the wallpaper programs are restarted.
#   sudo bash irisxe-wallpapertest.sh        the test (uses the built-in list: WallpaperSequoiaExtension, WallpaperMacintoshExtension)
#   sudo bash irisxe-wallpapertest.sh off    WallpaperSequoiaExtension without our device again (as v0.64)
#   sudo bash irisxe-wallpapertest.sh on     back to the built-in list (removes the file)
# Results: a folder irisxe-wallpaper-<time> and a zip of it in the same folder as this script.
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-wallpapertest.sh"; exit 1; fi
LIST=/Library/Preferences/irisxe-apps
U=$(stat -f %Su /dev/console)
restart_wp() {
    killall WallpaperSequoiaExtension WallpaperMacintoshExtension WallpaperVideoExtension WallpaperAgent 2>/dev/null
    sleep 2
}
case "$1" in
    off) printf '# none (irisxe-wallpapertest.sh off)\n' > "$LIST"; chmod 644 "$LIST"; restart_wp
         echo "== off: WallpaperSequoiaExtension no longer gets our Metal device. Choose the wallpaper again."; exit 0 ;;
    on)  rm -f "$LIST"; restart_wp
         echo "== on: built-in list again (WallpaperSequoiaExtension gets our Metal device)."; exit 0 ;;
esac
B=/Library/GPUBundles/IrisXeMTLDriver.bundle/Contents/Info.plist
V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$B" 2>/dev/null || echo none)
case "$V" in 0.6[6-9]*|0.[7-9]*|[1-9]*) ;; *) echo "The installed bundle is $V: install v0.66.0 first (bash install-bundle.sh)."; exit 1 ;; esac
OUT="irisxe-wallpaper-$(date +%H%M%S)"
mkdir -p "$OUT"
rm -f "$LIST"                                    # built-in list: WallpaperSequoiaExtension
echo "== bundle $V; restarting the wallpaper programs"
restart_wp
START=$(date "+%Y-%m-%d %H:%M:%S")
echo
echo "Now: open System Settings > Wallpaper, click the wallpaper to test (Sequoia, Macintosh ...) and wait 20 seconds."
echo "Look at the desktop: does the wallpaper change? Is it moving, still, black, or wrong colours?"
read -r -p "Press Return when done... " _
sleep 2
echo "== collecting"
log show --start "$START" --predicate 'process BEGINSWITH "Wallpaper" OR subsystem == "com.irisxe.IrisXeMTLDriver" OR sender CONTAINS "IrisXe"' \
    > "$OUT/log-all.txt" 2>&1
grep -iE "IrisXeMTL|SequoiaRenderer|error|fail|sandbox|deny|crash|metal|pipeline|shader|compile" "$OUT/log-all.txt" \
    | grep -v "display pipe: IrisXeDisplayPipe: showing" > "$OUT/log-important.txt"
log show --start "$START" --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "deny"' 2>/dev/null \
    | grep -i wallpaper > "$OUT/sandbox-deny.txt"
cp /Users/"$U"/Library/Logs/DiagnosticReports/Wallpaper* "$OUT/" 2>/dev/null
find /Library/Logs/DiagnosticReports -name "Wallpaper*" -newermt "$START" -exec cp {} "$OUT/" \; 2>/dev/null
pgrep -lf Wallpaper > "$OUT/processes.txt"
# v0.65.1: shaders our compiler refused (sandboxed programs save them in their own temporary folder)
mkdir -p "$OUT/failed-shaders"
cp /var/tmp/irisxe-fail/*Wallpaper* "$OUT/failed-shaders/" 2>/dev/null
find /Users/"$U"/Library/Containers /private/var/folders -path "*irisxe-fail/*" -name "*.bc" -newermt "$START" \
    -exec cp {} "$OUT/failed-shaders/" \; 2>/dev/null
{
    echo "bundle $V, start $START"
    echo "enabled in:";   grep -o "IrisXeMTL: enabled in [^ ]*.*" "$OUT/log-all.txt" | sort | uniq -c
    echo "hidden in:";    grep -o "IrisXeMTL: not enabled in [^ ]*" "$OUT/log-all.txt" | sort | uniq -c
    echo "renderer errors:"; grep -c "SequoiaRenderer.Error" "$OUT/log-all.txt"
    echo "sandbox denials:"; wc -l < "$OUT/sandbox-deny.txt"
    echo "crash reports:";   ls "$OUT" | grep -c -i "\.ips"
    echo "refused textures / resolves:"; grep -oE "newTexture type [0-9]+ format [0-9]+[^|]*|multisample resolve: .*" "$OUT/log-all.txt" | sort | uniq -c
    echo "failed shaders:";  grep -o "render pipeline FAILED: .*" "$OUT/log-all.txt" | sort | uniq -c
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
chown -R "$U" "$OUT"
zip -qr "$OUT.zip" "$OUT" && chown "$U" "$OUT.zip"
echo
echo "== done: send $OUT.zip (in $(pwd)) and tell me what the desktop showed."
echo "   If the wallpaper program misbehaves: sudo bash irisxe-wallpapertest.sh off"
