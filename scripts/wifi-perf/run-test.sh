#!/usr/bin/env bash
# Wi-Fi throughput run for an SG2002 board: iperf3 server on the board, reached
# over an independent control link (USB link-local through a jump host), and
# the iperf3 client on the jump host over the LAN/AP path. This is a lab
# harness, not a packaged tool: it expects a static riscv64 iperf3 at
# /run/sg2002-iperf3 and wifi-snap.sh at /run/wifi-snap.sh on the board.
# Usage: run-test.sh LABEL MODE SECONDS [board-pre-command]
#   MODE: rx (host->board), tx (board->host), bidir
# Env: JUMP (ssh host running the client), BOARD (ssh target through JUMP),
#      BOARD_IP (the board's Wi-Fi address as seen from JUMP), KNOWN_HOSTS.
# Writes runs/LABEL.{client.json,server.log,pre,post,ping} under this dir.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
label=$1; mode=$2; secs=$3; pre=${4:-true}
JUMP=${JUMP:-root@fuckup}
BOARD=${BOARD:-root@fe80::1a:11ff:fe00:101%usb0}
BOARD_IP=${BOARD_IP:-192.168.50.18}
K=${KNOWN_HOSTS:-$here/known_hosts}
bssh() { ssh -J "$JUMP" -i "$HOME/.ssh/id_rsa" -o IdentitiesOnly=yes -o UserKnownHostsFile="$K" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=20 -o LogLevel=ERROR "$BOARD" "$@"; }
out=$here/runs; mkdir -p "$out"
case $mode in
  rx) cflag="" ;;
  tx) cflag="-R" ;;
  bidir) cflag="--bidir" ;;
  *) echo "bad mode" >&2; exit 2 ;;
esac
bssh "iw dev wlan0 link | head -3; $pre" > "$out/$label.setup" 2>&1
grep -q "freq: 5180" "$out/$label.setup" || { echo "not on 5180 MHz" >&2; cat "$out/$label.setup"; exit 3; }
bssh "bash /run/wifi-snap.sh pre" > "$out/$label.pre"
bssh "nohup timeout $((secs+40)) /run/sg2002-iperf3 -s -1 -4 -p 5201 --idle-timeout 30 --rcv-timeout 10000 > /run/iperf-server.log 2>&1 &"
sleep 2
ssh "$JUMP" "ping -4 -i 0.5 -w $((secs+2)) $BOARD_IP" > "$out/$label.ping" 2>&1 &
pingpid=$!
ssh "$JUMP" "iperf3 -c $BOARD_IP -4 -p 5201 -t $secs -O 2 -J $cflag" > "$out/$label.client.json" 2>&1 || true
wait $pingpid || true
sleep 1
bssh "bash /run/wifi-snap.sh post" > "$out/$label.post"
bssh "cat /run/iperf-server.log" > "$out/$label.server.log" 2>&1 || true
# Summary line
python3 - "$out/$label.client.json" "$label" "$mode" "$secs" <<'EOF'
import json,sys
j=json.load(open(sys.argv[1])); e=j["end"]; lab,mode,secs=sys.argv[2:5]
def mb(x): return "%.2f" % (x/1e6)
if mode=="bidir":
    sr=e["sum_received"]; ss=e["sum_sent"]; rr=e["sum_received_bidir_reverse"]; rs=e["sum_sent_bidir_reverse"]
    st=e["streams"][0]["sender"]
    print(f"{lab} bidir {secs}s: board_rx={mb(sr['bits_per_second'])} Mbit/s (host retrans {ss.get('retransmits','?')}, rtt mean {st.get('mean_rtt',0)/1000:.1f} ms, max cwnd {st.get('max_snd_cwnd')}), host_rx={mb(rr['bits_per_second'])} Mbit/s (board retrans {rs.get('retransmits','?')})")
else:
    ss=e["sum_sent"]; sr=e["sum_received"]
    who="board_rx" if mode=="rx" else "host_rx"
    st=e["streams"][0]["sender"]
    print(f"{lab} {mode} {secs}s: {who}={mb(sr['bits_per_second'])} Mbit/s, sender retrans={ss.get('retransmits','?')}, rtt mean {st.get('mean_rtt',0)/1000:.1f} ms, max cwnd {st.get('max_snd_cwnd')}")
EOF
tail -1 "$out/$label.ping" | sed "s/^/$label ping: /"
