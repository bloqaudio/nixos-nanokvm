# SG2002 USB gadget validation: 2026-09-19

A LicheeRV Nano W (the "camera" board, USB port `3-1` of the x86 host
`strix-2`, kernel 7.2.0-rc2, xhci) ran a series of RAM-only USB initrd images
built from this repository with Linux 7.2.6. Every image was uploaded through
the ROM and mainline U-Boot's fastboot gadget; nothing was flashed. The host
side of every measurement was iperf 3.21 on `strix-2`; the board side was the
same iperf3 inside the initrd, reached over the gadget's IPv6 link-local
address on the host's `usb0`. Board CPU use is the delta of `/proc/stat`
around each run. Every throughput number below is a single 20-second sample
unless it says otherwise.

## The bulk-OUT wedge was not reproduced

The `usb-rx-guard` watchdog exists for a failure with a precise signature:
the board keeps transmitting but its `usb0` receive counter freezes, and only
a dwc2 platform-driver rebind recovers it. That signature was watched for on
every boot of this validation by a reporter service inside the image (a probe
ping every four seconds; three windows of TX progress without RX progress
trigger a dwc2 debugfs dump over the ACM function, then a rebind). It never
fired: not during roughly 40 minutes of saturating iperf3 traffic across
CDC-ECM, RNDIS and CDC-NCM at high-speed, not during six host-driven USB
suspend/resume cycles, and not during the six-boot U-Boot handoff loop.
Hardware counters (`rx_errors`, `rx_dropped`,
`tx_errors`) stayed at zero throughout except for one deliberate overload
noted under "UDP".

Because it did not reproduce, this document does not name its root cause.
The candidates that were examined and ruled out with evidence:

- **DMA unmap before the bounce copy.** dwc2 bounces any request whose buffer
  is not 4-byte aligned; on completion it calls `dwc2_hsotg_unmap_dma()` and
  then `dwc2_hsotg_handle_unaligned_buf_complete()`. On RISC-V with
  non-coherent DMA the unmap is the `arch_sync_dma_for_cpu()` cache
  invalidate, so copying after it is the correct order. The reverse would be
  a bug; the shipped order is not.
- **Slave/PIO mode.** Upstream forces `g_dma = host_dma = false` for this SoC.
  Repository patch 0001 already lifts that; with it the debugfs `params` show
  `g_dma = 1`, `g_dma_desc = 1`, and `hw_params` show `dma_desc_enable = 1`
  on a DWC_otg 4.20a core with 3072 words of FIFO RAM. This is the same
  buffer-plus-descriptor DMA mode the vendor 5.10 kernel forces. The 0001
  metadata note, which claimed the core reports no descriptor DMA, was wrong
  and is corrected.
- **The U-Boot handoff.** The initrd rebinds the dwc2 driver before claiming
  the UDC because the controller inherited from fastboot was seen to leave
  bulk-OUT dead. Six consecutive boots with that rebind disabled
  (`sg2002.usbGadget.initrd.resetController = false`) all came up with a
  working receive path (see "Handoff boot loop"). The rebind stays on by
  default; this shows it is not needed on this board and host, not that it
  is unnecessary elsewhere.
- **Upstream and vendor fixes.** `drivers/usb/dwc2` in mainline has no
  gadget data-path fix after 7.2.6 (the 2026 commits are a partial-power-down
  pull-up fix and a udc_stop spinlock fix). The vendor 5.10 tree's dwc2
  changes against v5.10.4 are role switching, partial power down, remote
  wakeup, a charger-detection PHY sequence and the `cv182x` parameter set;
  none touches OUT-transfer handling.

The full-speed cap that the shipped DT carried since the 2026-08 PicoClaw
bring-up was the dominant performance limit, and the historical wedge reports
came from that carrier at high-speed with `-71 EPROTO` enumeration errors.
That carrier was not available for this validation and keeps full-speed.

## Throughput

`iperf3 -c <board> -t 20 -i 0` from the host, and `-R` for the reverse
direction. "Host to board" is USB bulk OUT, the direction the wedge affected.

| Image | Host to board | Board to host | Board CPU (H2B / B2H) |
| --- | ---: | ---: | --- |
| master `69338ec`, CDC-ECM, full-speed | 7.69 Mbit/s | 8.38 Mbit/s | 18% / 2% |
| same kernel, CDC-ECM, high-speed DT | 208 Mbit/s | 152 Mbit/s | 100% / 100% |
| + dwc2 RX-buffer patch (0076) | 222 Mbit/s | 154 Mbit/s | 100% / 100% |
| + FIFO layout 1024 / 512 512 480 480 16 16 (shipped) | 222 Mbit/s | 184 Mbit/s | 100% / 100% |
| FIFO layout 960 / 512 512 512 512 16 16 (not shipped) | 222 Mbit/s | 188 Mbit/s | 100% / 100% |
| shipped kernel and FIFOs, CDC-NCM | 231 Mbit/s | 261 Mbit/s | 99% / 100% |
| pre-patch kernel and old FIFOs, CDC-NCM | 187 Mbit/s | 262 Mbit/s | 100% / 100% |
| pre-patch kernel and old FIFOs, RNDIS | 191 Mbit/s | 134 Mbit/s | 100% / 100% |

Four parallel host-to-board streams reached the same aggregate as one
(215-231 Mbit/s) in every high-speed configuration. The full-speed baseline
was also soaked with twelve consecutive 16 MiB `ssh` pipes into the board at
7.70-7.71 Mbit/s each, with the receive counter advancing every round.

The two FIFO layouts differ by one sample's noise; the shipped one keeps the
ACM bulk-IN endpoint on a 1920-byte FIFO. RNDIS was not re-measured on the
final kernel. Board CPU is saturated in every high-speed case, with softirq
at 70-90% of it for receive; the single 1 GHz C906 is the limit, not the
link.

## UDP

`iperf3 -u -l 1400` at a fixed offered rate. At full-speed, 12 Mbit/s
offered into the board lost 30%. At high-speed, 100 Mbit/s offered in either
direction was delivered without loss on the shipped configuration (CDC-ECM
at 94% board CPU, CDC-NCM at 68%). Before the RX-buffer patch, the same
100 Mbit/s test lost 0.93% (RNDIS) and 1.2% (CDC-NCM). One deliberate
overload, 150 Mbit/s into the unpatched CDC-ECM image, was almost entirely
dropped at the board's network backlog (`rx_dropped` 263640) and is the only
non-zero error counter of the day.

## Suspend and resume

With `power/control` on the host set to `auto`, six cycles were run on the
unpatched high-speed CDC-ECM image: three where the host device stayed
`active`, and three where taking the host interface down let the device
reach `suspended` (a real USB suspend on the gadget side) before bringing it
up again. Every cycle was followed by three pings and a five-second TCP run;
all pings answered and every run delivered 201-206 Mbit/s.

## Handoff boot loop

Six consecutive ROM-to-Linux uploads of the patched high-speed CDC-ECM image
with the initrd's dwc2 driver rebind disabled. After each boot the host
pinged the board (six of six answered), the board's `usb0` receive counter
had advanced (129-147 packets), and the reporter's boot snapshot showed the
bulk-OUT endpoint enabled (`DOEPCTL 0x80098200`) with no wedge report. The
planned eight-plus-four loop and the one-hour soak were cut short: `strix-2`
went down mid-loop (its seventh boot lost the host), and the work was wrapped
up on a budget before it returned. The longest continuous traffic on one
boot was therefore the ~10 minutes of back-to-back iperf3 batteries per
image, not a dedicated soak, and the initrd images carry no `rxGuard` (it
is a stage-2 option, off by default).

## CDC-NCM segment size, not shipped

`f_ncm`'s `max_segment_size` was set to 8000 on one boot to try 7986-byte
frames. ICMP of every size up to 7900 bytes crossed in both directions, and
the board sent TCP segments larger than 1500 bytes, but `ssh` from the host
stalled after roughly 10 KB with the host's socket showing `pmtu:68 mss:36`
and ten retransmission backoffs while pings continued. The board was rebooted
by the next test before the cause was found, so this is recorded as
undetermined and the knob is not exposed.

## Checks

On the final tree, rebased onto master `e67f250`: `nix flake check
--no-build` reports all checks passed once an authorized-keys file is
supplied (it first failed on master's dev shell naming the removed
`go_1_25`, fixed here in its own commit); the `sg2002-initrd-boot` QEMU boot
test, `sg2002-initrd-eval`, the uploader unit tests
(`sg2002-usb-boot-runner`, `sg2002-c906l-runner`) and the
`hydraJobs.x86_64-linux.ci.checks` aggregate all built. The profiling
(`perf`) image variant could not be built: the tool pushes the initrd past
its 80 MiB unpacked budget, so CPU accounting comes from `/proc/stat` only.

## Identity and limits

- Baseline FIT (master `69338ec`, full-speed CDC-ECM): 32,637,276 bytes,
  SHA-256 `ed0d7a9c1d16d9e13010bcba97dff651321283fbf8bc4f6e015490610e17b79c`.
- Final-configuration FIT built from this tree before the rebase
  (`boards.licheerv.mainline.initrd.default`): 32,626,872 bytes, SHA-256
  `53479f2e864ce6ec2f2e8399dcbd54d601194179dafae2e21bb86def1e3c2d8e`;
  kernel `Image` SHA-256
  `135089192dbee9b918985057b4db87414c40fd80b9910354b0f8136f3c412475`;
  DTB SHA-256
  `a5da2d7bcc45f6b77c23030d44ebe2630867cc553f12ebdfe7cb8620bedb9ea1`
  (`maximum-speed = "high-speed"`, `g-rx-fifo-size = <1024>`,
  `g-tx-fifo-size = <512 512 480 480 16 16>`). The measurement images were
  the same configuration plus iperf3, busybox and the reporter service.
- The plain final FIT was built and hashed but not itself booted; every
  boot used a measurement image.

- One board, one host port, one cable. The PicoClaw and NanoKVM-PCIe
  carriers were not tested; PicoClaw keeps full-speed, PCIe moves to
  high-speed with the same silicon and description but without its own
  measurement.
- All numbers are single samples; the ECM host-to-board figure repeated at
  222 Mbit/s across three images, the rest did not get a second run.
- The wedge was not reproduced, so nothing here proves it fixed. The
  `rxGuard` option is retained, off by default, for that reason.
- The stage-2 SD image path and its `reenumerateAfterBoot` timer were not
  exercised; this validation is RAM-only stage 1.
