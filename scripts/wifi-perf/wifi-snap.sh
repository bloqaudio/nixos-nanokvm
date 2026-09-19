#!/bin/bash
# Counter snapshot for the PicoClaw Wi-Fi tests. Runs on the board.
# Usage: wifi-snap.sh LABEL   (prints one block; diff two blocks offline)
BB=$(ls -d /nix/store/*busybox*/bin/busybox | head -1)
label=${1:-snap}
echo "=== $label $(date -u +%FT%TZ) uptime=$(cut -d' ' -f1 /proc/uptime)"
echo "--- cpu"; head -1 /proc/stat
echo "--- softirqs"; cat /proc/softirqs
echo "--- irq mmc1"; $BB grep -E "mmc1|IRQ work" /proc/interrupts
echo "--- netdev wlan0"; $BB grep wlan0 /proc/net/dev
echo "--- threads (pid comm policy rtprio utime stime)"
for p in /proc/[0-9]*; do
  c=$(cat "$p/comm" 2>/dev/null) || continue
  case "$c" in aicwf*|ksoftirqd*|kworker/0:*|kworker/R-sdhci|sg2002-iperf3|iperf3|wpa_supplicant|kworker/u*)
    set -- $(cat "$p/stat" 2>/dev/null | $BB sed 's/^.*) //')
    # after stripping "pid (comm) ", fields: state=1 ... utime=12 stime=13 policy=39 rtprio=38
    echo "$(basename "$p") $c policy=${39} rtprio=${38} utime=${12} stime=${13}";;
  esac
done
echo "--- station"; iw dev wlan0 station dump 2>/dev/null
echo "--- link"; iw dev wlan0 link 2>/dev/null; iw dev wlan0 get power_save 2>/dev/null
if [ -r /sys/kernel/debug/ieee80211/phy0/rwnx/stats ]; then
  echo "--- rwnx stats"; cat /sys/kernel/debug/ieee80211/phy0/rwnx/stats
fi
echo "--- thermal"; cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null
echo "--- cpufreq"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null
echo "=== end $label"
