# PicoClaw Wi-Fi throughput investigation: 2026-09-19

This report records what was measured on the SG2002 PicoClaw about the
asymmetric Wi-Fi throughput documented in `sg2002-performance.md`, which
mechanisms were established, and which were ruled out. It makes **no
throughput claim for any code change**: no driver or kernel change was found
that improves the measured cases, and none was committed. The value here is
the explanation and the list of hypotheses that no longer need re-testing.

## Setup and identities

- Board: PicoClaw on host `fuckup` (USB 10-4), running this worktree's
  standalone RAM image: `boot.itb` SHA-256
  `7dbf1f49a834d8a9d6a28bbf7f0cedec0d4a22a85a0e8fd5913114e7e372d74b`,
  Linux 7.2.6, `aic8800_fdrv.ko` SHA-256 `93117e60…e3e0` (radxa-pkg/aic8800
  `bd11969`, `CONFIG_FILTER_TCP_ACK=n`, no Bluetooth), C906 at 1 GHz,
  `schedutil`, Wi-Fi power saving on throughout.
- Access point: "Radio Free Europe", BSSID `80:2a:a8:82:96:ef`, 5180 MHz,
  WPA3-SAE with PMF required, RSSI −68 dBm at the board. Every run below is
  on this BSS; the band was checked by the harness before each run.
- Path: the iperf3 client ran on `fuckup` (`192.168.23.7`); the board's
  address `192.168.50.18` is routed through `192.168.23.1` to the AP's
  network. The router was not reachable for inspection.
- Tool: static iperf 3.21 on the board (`pkgsStatic.iperf3`, store path
  `53fk0pbsk9mx1n2vkdiw9nxvkxa5mq9s-iperf-static-…-3.21`), iperf 3.21 on the
  host. TCP runs are 20 s after a 2 s omitted warm-up; UDP runs 15 s.
  Counters were snapshotted on the board before and after each run
  (`scripts/wifi-perf/`). USB was the control connection; Wi-Fi never
  dropped during the session.
- Rates the AP used toward the board (driver `rc/*/rx_rate`, whole session):
  62 % VHT40 SGI MCS7, 34 % VHT40 LGI MCS6. Board transmit rate VHT40 LGI
  MCS8 (162 Mbit/s).

Credentials: the July `wifi.conf` in the main checkout is rejected by this
AP (SAE confirm status 15 on both bands, which is a passphrase rejection,
not a key-management problem — the SAE exchange itself completes). The
working secret is the sops-managed `wifi-password` used by the user's
NixOS configurations; it was streamed from `/run/secrets/wifi-password` on
`fuckup` into the board's RAM and is not stored in this repository.

## Baseline (one sample each)

| Test | Result |
| --- | --- |
| Receive, host→board | 54.04 Mbit/s, 441 sender retransmits, mean RTT 7.5 ms, max cwnd 72 KB |
| Transmit, board→host | 110.15 Mbit/s, 0 retransmits; ping RTT during the run 53 ms mean |
| Bidirectional | board received 15.57 Mbit/s (62 retransmits, RTT 37 ms), host received 82.8 Mbit/s |

Later bidirectional samples on the same setup were bimodal: 4.2–8.3 Mbit/s
received with 78–96 ms RTT while the host received 93–103, or 15.4–15.6
received with 29–41 ms RTT while the host received 83–84. This reproduces
the documented collapse (3.13 Mbit/s) in kind. Nine bidirectional samples
were taken in total during the knob tests below.

## What the counters showed

- CPU is not the limit: 41 % idle in the receive-only run, 55 % in
  bidirectional. `aicwf_busrx_thread` used 604 ticks (of 100 Hz) in the
  receive run and 508 in bidirectional; `aicwf_bustx_thread` 203 and 349;
  the `system_wq` kworkers that service the SDIO interrupt 55 ticks each.
  Both driver threads are SCHED_FIFO priority 1 as the source says; the
  kworkers still got the CPU, and the card handed over only ~1.3 frames per
  SDIO read in receive-only and ~2 in bidirectional, so no receive backlog
  was building in the card. The "TX thread starves the RX kworker" theory
  is therefore **not supported by measurement**.
- The board's transmit queues were empty: the cfg80211-side `txq` debugfs
  view showed zero ready frames throughout a bidirectional run (sampled at
  ~2 Hz), while the sending socket held 1.36–1.54 MB unacknowledged. That
  data was on the air or at the AP, not in the board.
- The A-MPDU hole counter (`#mpdu missed`, `rwnx_rx.c:256`) counts gaps
  inside received aggregates, not final loss: two UDP runs produced 1490
  holes against 290 lost datagrams.
- Sender-side TCP counters on `fuckup` for a receive-only run: 441
  `TCPFastRetrans` in 128 `TCPSackRecovery` episodes, **0** `TCPDSACKRecv`,
  0 reordering events. The retransmitted segments really had not arrived;
  they were not late deliveries from the driver's reorder buffer.

## Mechanism 1: the bidirectional collapse is airtime

With both directions capped at the sender (`iperf3 --bidir -b`), 20 s each,
one sample per cap:

| Cap per direction | Board received | Host received | RTT (host→board stream) |
| --- | --- | --- | --- |
| none | 4.2–15.6 Mbit/s | 83–103 Mbit/s | 29–96 ms |
| 50 Mbit/s | 27.2 Mbit/s | 50.0 Mbit/s | 10.7 ms |
| 30 Mbit/s | 30.0 Mbit/s | 30.0 Mbit/s | 6.3 ms |

The receive direction recovers as the board's uplink is throttled. An
uplink of 100–110 Mbit/s on a 162 Mbit/s single-stream PHY occupies most of
a half-duplex channel; the AP's downlink — the host's data and the board's
own ACKs — gets the remainder, and the ~90 ms is the AP's queue for this
station. Nothing in the board's software queues this data, so no board-side
queue discipline can shorten it. The same effect explains the 53 ms ping RTT
during a transmit-only run.

## Mechanism 2: receive-only TCP is probing a ~74 Mbit/s air bottleneck

UDP toward the board: 60 Mbit/s offered arrived at 60 with 0 of 80,355
datagrams lost; 90 Mbit/s offered arrived at 90 with 290 of 120,535 lost
(0.24 %). The receive pipe — air, SDIO, driver — is therefore not a
54 Mbit/s ceiling. With the TCP sender paced (`iperf3 --fq-rate`), one 20 s
sample each:

| Sender pacing | Board received | Sender retransmits | Mean RTT |
| --- | --- | --- | --- |
| none | 54.04 Mbit/s | 441 | 7.5 ms |
| 60 Mbit/s | 59.80 Mbit/s | 0 | 7.2 ms |
| 80 Mbit/s | 73.80 Mbit/s | 179 | 34.8 ms |
| 95 Mbit/s | 74.07 Mbit/s | 3 | 40.9 ms |

Unpaced cubic grows its window until the ~74 Mbit/s the air path affords a
TCP flow here (the ACK uplink costs airtime that a UDP flow does not pay),
loses at the overrun, halves, and averages 54. The 35–41 ms RTT at the
higher pacing rates is queueing at the AP. The loss itself is over the air
or in the AP; with the sender held below the bottleneck there is none.
A concurrent uplink of ~1950 small datagrams per second — more than the
~740 ACKs per second of a TCP receive — caused no downlink loss to a
60 Mbit/s UDP flow, so the board's ACK traffic is not what loses the frames.

## Ruled out on this board (each A/B/A/B unless stated)

| Hypothesis | Test | Result |
| --- | --- | --- |
| SCHED_FIFO TX thread starves the RX kworker | per-thread CPU, IRQ rate, frames per SDIO read | not supported; see above |
| TCP small-queue accounting lets the board's TX queue bloat | `tcp_limit_output_bytes` 4 MiB vs 64 KiB, bidirectional, A/B/A/B/A | 4.75 / 5.08 / 4.38 / 4.70 / 8.31 Mbit/s received; no effect |
| qdisc backlog on `wlan0` | `txqueuelen` 1000 vs 32, bidirectional, A/B/A/B | 15.58 / 4.91 / 4.22 / 15.35; bimodal, unrelated to the knob |
| Driver reorder timer holds frames too long | `reorder_timeout` 50 vs 10 ms, receive-only, A/B/A/B | 52.0 / 51.7 / 51.6 / 52.0 Mbit/s, 367–464 retransmits; no effect |
| Driver reorder buffer causes spurious retransmits | sender `TCPDSACKRecv`, `TCPSackReorder` | both zero; losses are real |
| RX DMA buffer bounce/alignment | source: `__dev_alloc_skb` aggregate buffer, 64-byte aligned | no bounce path found |
| TCP data copied on transmit, blinding TSQ | `sizeof(struct rwnx_txhdr)` ≈ 88 B < TCP headroom | not copied |
| Receive pipe capacity | UDP 60/90 Mbit/s | 60 / 90 Mbit/s delivered |
| Board ACK uplink loses downlink frames | UDP down 60 Mbit/s with 1950 small uplink datagrams/s | 0 loss |

The Wi-Fi power-saving comparison in `sg2002-performance.md` remains
inconclusive; it was not repeated.

## What this does and does not establish

It establishes that on this AP, band and signal the board's receive
throughput is bounded by the medium and the AP's behaviour, and that the
bidirectional collapse is the board's own uplink consuming the channel. It
does not establish the behaviour at other signal levels, on the 2.4 GHz BSS,
with an AP that enforces airtime fairness, or with the LCD/DRM load present
in the documented 3.13 Mbit/s run, where CPU contention may add a second
effect. The 4.2–15.6 Mbit/s bimodality between otherwise identical
bidirectional samples is unexplained; it may be AP rate-control state.
Single-sample numbers above are single samples.

Possible board-side follow-ups, none measured: GRO on the receive path
(fewer ACK transmissions and less TCP work per byte, worth at most the
~7 % airtime the ACK uplink costs), and a transmit rate or aggregation limit
if a duplex-fair link matters more than uplink throughput — which is a
policy choice, not a fix.

The board was left running this image, associated on 5180 MHz, with the
static iperf3 and snapshot script in `/run`; nothing was flashed. Host
`fuckup` kept its usual `claw-usb-boot` autoloader active.
