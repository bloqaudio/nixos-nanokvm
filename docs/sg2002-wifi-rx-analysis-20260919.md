# SG2002 Wi-Fi receive path: driver analysis, 2026-09-19

This is a code-reading and board-inspection note, not a measurement report.
No new throughput figures were taken: the session ended before the board
could be associated, because the locally held July credential is no longer
accepted by the access point (see "Blocked" below). Everything here either
cites a source line or a read-only observation on the running PicoClaw. The
bidirectional receive collapse recorded in `sg2002-performance.md` is **not
explained** by this note; two testable hypotheses are set out at the end.

Driver source: radxa-pkg/aic8800 `bd11969`, SDIO tree
`src/SDIO/driver_fw/driver/aic8800/`, as built by
`pkgs/sg2002/aic8800-mainline`. Kernel 7.2.6 with the repository's
`sdhci-of-dwcmshc` SDIO1 patch.

## How receive actually flows

1. The card raises the SDIO DAT1 interrupt. The SDHCI host advertises
   `MMC_CAP_SDIO_IRQ | MMC_CAP2_SDIO_IRQ_NOTHREAD`
   (`drivers/mmc/host/sdhci.c`, `sdhci_setup_host`), so `sdhci_irq` masks the
   card interrupt and calls `sdio_signal_irq`, which queues `sdio_irq_work`
   on `system_wq`. There is no dedicated `ksdioirqd` thread on this host: the
   card's interrupt is serviced by an ordinary CFS kworker.
2. `sdio_run_irqs` claims the host and, because `aic8800_bsp` registers the
   handler through `sdio_claim_irq(func, NULL)` plus a direct
   `func->irq_handler` assignment (`aic8800_bsp/aicsdio.c:1383-1387`), calls
   `aicwf_sdio_hal_irqhandler` (`aic8800_fdrv/aicwf_sdio.c:2853`). That
   handler performs a CMD52 read of the block-count register and then a
   synchronous CMD53 `sdio_readsb` of one whole aggregate (up to 63 blocks of
   512 bytes, or byte mode) **inside the kworker**. Only after the copy
   lands does it enqueue the buffer and `complete(&busrx_trgg)`.
3. `aicwf_busrx_thread` (`aicwf_sdio.c:2695`) wakes, and
   `aicwf_process_rxframes` (`aicwf_txrxif.c:284`) walks the aggregate,
   allocating a fresh skb and `memcpy`-ing every frame out of it
   (`aicwf_txrxif.c:397-406`). `CONFIG_PREALLOC_RX_SKB` is `n` in the
   Makefile, so the aggregate buffer itself is `__dev_alloc_skb(size)`,
   whose data pointer is `NET_SKB_PAD`-aligned (64 bytes here). The DMA
   buffer is therefore aligned; the `dwc2`-style bounce-buffer tax found on
   the USB path does **not** have an obvious counterpart in this SDIO path.
   The per-frame copy is real but linear in bytes (about 7 MB/s at
   55 Mbit/s).
4. `rwnx_rxdataind_aicwf` (`rwnx_rx.c:2219`) rebuilds an Ethernet header,
   then for QoS data with `flags_need_reord` set hands the frame to the
   driver's own block-ack reorder buffer (`reord_process_unit`,
   `rwnx_rx.c:1931`): window 64 (`AICWF_REORDER_WINSIZE`), release timer
   `reorder_timeout` = 50 ms, timer work scheduled on `system_wq`
   (`rwnx_rx.c:1913`), the same queue the SDIO interrupt work uses. Frames
   are delivered with `netif_receive_skb` under `local_bh_disable`
   (`CONFIG_RX_NETIF_RECV_SKB=y`, `rwnx_rx.c:1790-1793`), so `NET_RX` runs
   inline in the busrx thread.

## How transmit flows, and where it can interfere

- `aicwf_bustx_thread` (`aicwf_sdio.c:2564`) → `aicwf_sdio_tx_process`
  (`:1930`) → `aicwf_sdio_flow_ctrl` (`:355`). Flow control is a CMD52
  read of the firmware's free-buffer count; when it is at or below
  `DATA_FLOW_CTRL_THRESH` (2), the thread **busy-waits**: `udelay(200)` for
  the first 30 attempts (6 ms of spinning), then `msleep(2)` for ten, then
  `msleep(10)`, up to `FLOW_CTRL_RETRY_COUNT` (50). A saturating TX stream
  keeps the firmware's buffers full, so this loop is the steady state of
  the TX thread during the "received by host" half of every bidirectional
  test. Each poll also claims and releases the MMC host, contending with
  the RX kworker's CMD53.
- Both driver threads are real-time. `CONFIG_TXRX_THREAD_PRIO=y` with
  `bustx_thread_prio = busrx_thread_prio = 1` (`aicwf_sdio.c:2461-2465`)
  makes each call `sched_set_fifo_low()`. Confirmed on the running board:
  `/proc/114/stat` and `/proc/115/stat` (the `aic8800_fdrv` threads) show
  policy 1, rt_priority 1, while every kworker, `ksoftirqd/0` and the
  `sdhci` workers are policy 0. On a single C906 hart a SCHED_FIFO thread
  spinning in `udelay` cannot be preempted by the CFS kworker that has to
  read the card's receive FIFO.
- TX aggregation is `CONFIG_SDIO_ADMA=n`: frames are copied into one
  `MAX_AGGR_TXPKT_LEN` (1536×64) buffer and written with a single
  `sdio_writesb`, which the MMC core splits into CMD53s of at most 511
  blocks. Per-station credits start at `NX_TXQ_INITIAL_CREDITS` (64).
- The kernel has `NET_SCHED = no` (`pkgs/sg2002/linux-mainline/config.nix`),
  so no fq_codel or any other AQM can be attached to `wlan0`; the board's
  own TCP ACKs for the receive stream queue behind bulk data in the same
  BE queue.

## Two hypotheses for the bidirectional collapse (untested)

1. **Real-time TX polling starves the CFS RX kworker.** Predicts: during
   bidirectional traffic `aicwf_bustx_thread` accrues large CPU time in
   `/proc/<pid>/stat`, `mmc1` interrupt rate drops, and the collapse
   shrinks when the thread is moved to CFS at runtime
   (`busybox chrt -o -p 0 <pid>`, reversible, no rebuild). A durable fix
   would replace the `udelay` spin with `usleep_range` and default both
   priorities to 0 (module parameters already exist, but they are read
   only at thread start).
2. **ACK head-of-line blocking behind bulk TX with no AQM.** Predicts:
   RTT measured with `ping` during the bidirectional test inflates to tens
   or hundreds of milliseconds; a receive-only test does not show this.
   Mitigations would be enabling `NET_SCHED` with fq_codel in the kernel
   configuration, or driver-side ACK prioritisation.

The two are not exclusive. Neither has been measured. Nothing in this note
should be quoted as a cause.

## Blocked: association credential

The gitignored `wifi.conf` in the main checkout (dated 2026-07-30) is
rejected by the "Radio Free Europe" access point on both its 5180 MHz and
2412 MHz BSSes. The AP is WPA3-only (`[WPA2-SAE+FT/SAE-CCMP][SAE-H2E]`); with
`key_mgmt=SAE ieee80211w=2` the SAE commit is accepted (status 126 followed
by an H2E retry) and the confirm is rejected with status 15, the signature
of a passphrase mismatch. A supplicant configuration for the same SSID
should exist in the closure of the NFS live system that `claw-usb-boot` on
`fuckup` boots; candidate store paths on `fuckup` include
`/nix/store/6zks6jw9ahnn5xp9xsdgqvdbarcc1wbg-wpa_supplicant.conf` and the
`*-etc-wpa_supplicant-nixos.conf` entries. They were not read in this
session. Never commit the credential.

## Board and artifact state

- The PicoClaw is still running the image left by the previous session
  (kernel 7.2.6 #1 2026-09-14). Its `aic8800_fdrv.ko` has SHA-256
  `7bb45a86…416c`, which matches none of the four driver variants this
  worktree builds (`93117e60…`, `d9546a9f…`, `26dbc443…`, `85216ee9…`), so
  its driver provenance is unknown. Its `/etc/wpa_supplicant.conf` was
  restored to the content found, and the temporary `/run/sg2002-iperf3`
  was removed. `claw-usb-boot.service` on `fuckup` was never stopped.
- A baseline bundle with this worktree's driver was built but **not
  uploaded**: `boot.itb` SHA-256
  `7dbf1f49a834d8a9d6a28bbf7f0cedec0d4a22a85a0e8fd5913114e7e372d74b`
  (`artifacts/wifi-perf/bundle-baseline`, untracked).
- The previous session's static test binary is still available:
  `/nix/store/53fk0pbsk9mx1n2vkdiw9nxvkxa5mq9s-iperf-static-riscv64-unknown-linux-musl-3.21/bin/iperf3`.
  The recorded method was `iperf3 -s -1 -4` on the board with the client
  on `fuckup` (192.168.23.7) reaching the board's 192.168.50.x address.

## Suggested next step

Boot the baseline bundle, associate on 5180 MHz with a verified SAE
credential, then run one 20-second bidirectional test while sampling
`/proc/<pid>/stat` for the two driver threads and the `sdio`/`events`
kworkers, `/proc/interrupts` (`mmc1`) and `ping` RTT. Repeat once with
`chrt -o -p 0` applied to `aicwf_bustx_thread`, then restore FIFO. That
single reversible pair discriminates hypothesis 1 without a rebuild.
