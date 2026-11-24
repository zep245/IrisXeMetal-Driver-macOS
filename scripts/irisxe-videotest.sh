#!/bin/bash
# IrisXe v0.67.0 video engine test (hardware video decoding, step V1): does Tiger Lake's video engine (VCS0) wake up
# and run a job? Runs one tiny job on it (no video yet) and shows the report. Safe: the engine is stopped again if
# the job does not run; nothing else changes. No restart needed afterwards.
#   sudo bash irisxe-videotest.sh
cd "$(dirname "$0")" || exit 1
if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash irisxe-videotest.sh"; exit 1; fi
if ! sysctl -n debug.irisxe_vcstest > /dev/null 2>&1; then
    echo "IrisXeAccel is older than v0.67.0: run bash install-all.sh from this release, allow it in Privacy & Security, restart, run this again."
    exit 1
fi
OUT="irisxe-video-$(date +%H%M%S).txt"
{
    echo "== versions"; kextstat 2>/dev/null | grep -i irisxe | awk '{print $6, $7}'
    echo "== test 1"
    sysctl debug.irisxe_vcstest=1
    ioreg -l -w0 | grep -o '"IrisXe-VCSTest" = "[^"]*"'
    sleep 1
    echo "== test 2 (again: the engine must run a second job too)"
    sysctl debug.irisxe_vcstest=1
    ioreg -l -w0 | grep -o '"IrisXe-VCSTest" = "[^"]*"'
    echo "== kernel log"
    log show --last 2m --predicate 'sender CONTAINS "IrisXe"' 2>/dev/null | grep -iE "VCS|hang|reset" | tail -20
} 2>&1 | tee "$OUT"
U=$(stat -f %Su /dev/console); chown "$U" "$OUT"
echo
echo "== done: send $OUT (in $(pwd))"
