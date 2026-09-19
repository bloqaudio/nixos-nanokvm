#!/usr/bin/env python3
"""Diff two wifi-snap.sh blocks: CPU, softirqs, mmc1 IRQs, per-thread CPU."""
import re, sys

def parse(path):
    d = {"threads": {}, "softirq": {}, "irq": {}, "cpu": None, "netdev": None, "sta": {}}
    sec = None
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("--- "):
            sec = line[4:]; continue
        if sec == "cpu" and line.startswith("cpu "):
            d["cpu"] = [int(x) for x in line.split()[1:]]
        elif sec == "softirqs":
            m = re.match(r"\s*(\w+):\s+(\d+)", line)
            if m: d["softirq"][m.group(1)] = int(m.group(2))
        elif sec == "irq mmc1":
            m = re.match(r"\s*(\S+):\s+(\d+)", line)
            if m: d["irq"][m.group(1)] = int(m.group(2))
        elif sec == "netdev wlan0" and "wlan0" in line:
            d["netdev"] = [int(x) for x in line.split(":")[1].split()]
        elif sec and sec.startswith("threads"):
            m = re.match(r"(\d+) (\S+) policy=(\d+) rtprio=(\d+) utime=(\d+) stime=(\d+)", line)
            if m:
                pid, comm, pol, rt, ut, st = m.groups()
                d["threads"][pid] = (comm, int(pol), int(rt), int(ut), int(st))
        elif sec == "station":
            m = re.match(r"\s*(tx retries|tx failed|rx drop misc|rx packets|tx packets|rx bytes|tx bytes):\s*(\d+)", line)
            if m: d["sta"][m.group(1)] = int(m.group(2))
    return d

a, b = parse(sys.argv[1]), parse(sys.argv[2])
if a["cpu"] and b["cpu"]:
    names = ["user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"]
    delta = [y - x for x, y in zip(a["cpu"], b["cpu"])]
    tot = sum(delta[:8])
    print("cpu ticks total=%d  " % tot + "  ".join("%s=%d(%.0f%%)" % (n, v, 100.0 * v / tot) for n, v in zip(names, delta[:8])))
print("softirq deltas: " + "  ".join("%s=%d" % (k, b["softirq"][k] - a["softirq"].get(k, 0)) for k in b["softirq"] if b["softirq"][k] - a["softirq"].get(k, 0)))
print("irq deltas: " + "  ".join("%s=%d" % (k, b["irq"][k] - a["irq"].get(k, 0)) for k in b["irq"]))
if a["netdev"] and b["netdev"]:
    n = [y - x for x, y in zip(a["netdev"], b["netdev"])]
    print("wlan0 rx bytes=%d pkts=%d errs=%d drop=%d | tx bytes=%d pkts=%d errs=%d drop=%d" % (n[0], n[1], n[2], n[3], n[8], n[9], n[10], n[11]))
if a["sta"] and b["sta"]:
    print("station: " + "  ".join("%s=%d" % (k, b["sta"][k] - a["sta"].get(k, 0)) for k in b["sta"]))
print("threads (ticks: utime+stime delta), policy/rtprio:")
rows = []
for k, (comm, pol, rt, ut, st) in b["threads"].items():
    if k in a["threads"]:
        _, _, _, ut0, st0 = a["threads"][k]
        dd = (ut - ut0) + (st - st0)
        if dd: rows.append((dd, k, comm, pol, rt))
for dd, pid, comm, pol, rt in sorted(rows, reverse=True):
    print("  %5d  pid=%-5s %-28s policy=%d rtprio=%d" % (dd, pid, comm, pol, rt))
