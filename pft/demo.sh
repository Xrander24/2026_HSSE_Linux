#!/bin/sh
# pft demo: load module, capture some events, print stats.
# Designed to run inside the busybox initramfs (sh + standard utils only).

set -u

echo "=== pft demo ==="

# debugfs may or may not already be mounted by init
mount -t debugfs none /sys/kernel/debug 2>/dev/null

if ! lsmod 2>/dev/null | grep -q '^pft '; then
    echo "--- insmod /pft.ko"
    insmod /pft.ko
fi

# Reader writes a capped number of events to a log file in the background.
# 200 events is enough for a demo and finishes in <1s under normal workload.
echo "--- starting pft-ctl in background"
rm -f /tmp/pft.log
/pft-ctl -n 200 > /tmp/pft.log &
READER=$!

# Give pft-ctl time to open the fd before we generate workload.
sleep 1

echo ""
echo "--- workload: read-only first-touch (demand paging)"
cat /init           > /dev/null
ls -la /bin         > /dev/null

echo "--- workload: fork + write -> copy-on-write"
# Each child writes to a shell variable, dirtying shared pages.
sh -c '
    i=0
    while [ $i -lt 5 ]; do
        ( j=0; while [ $j -lt 50 ]; do j=$((j+1)); done ) &
        i=$((i+1))
    done
    wait
'

echo "--- workload: instruction-fetch (exec)"
/bin/true
/bin/echo done > /dev/null

# Wait for the reader to fill its quota (or 2s, whichever comes first).
wait $READER 2>/dev/null

echo ""
echo "================== /proc/pft/stats =================="
cat /proc/pft/stats
echo "====================================================="
echo ""
echo "----- first 15 events from /tmp/pft.log -----"
head -16 /tmp/pft.log
echo ""
echo "events captured: $(wc -l < /tmp/pft.log)"
echo ""
echo "(rmmod pft   to unload; dmesg | tail   for summary)"
