# SG2002 performance and profiling

Optimize measured bottlenecks before changing clock limits. The SG2002 has
one Linux C906 hart, little RAM, and separate hardware video engines; faster
CPU code does not automatically mean faster SD, USB or hardware encoding.

## Compiler configuration

The CV181x NixOS platform selects `gcc.tune = "thead-c906"`. This supplies
GCC's scheduling/cost model throughout the target package set, including
libc, without changing the instruction set or ABI. Native build tools keep
their own target. Rust, Go, prebuilt binaries and the independently packaged
C906L firmware are not implicitly retuned by this GCC setting.

The pinned nixpkgs compiler wrapper omits RISC-V from its supported-tuning
dispatch. `pkgs/sg2002/c906-tuning.nix` uses the wrapper's extension point to
honour this explicit platform setting. The flag precedes package arguments,
so an explicit package override remains possible. The regression check
inspects the final and libc-bootstrap wrappers, asks GCC which tuning is
active, checks a caller override, and runs C and C++ programs under the C906
QEMU model. It also checks that vector instructions were not enabled.

This changes derivation identities across the target closure and therefore
requires rebuilding packages that a generic RISC-V binary cache cannot supply.
It is not `-march=native`, an overclock, an ABI change, or blanket `-O3`/LTO.

The tuned closure builds for all four RAM-image variants and all three
persistent SD images. The tuning checks and boot/peripheral checks are
exposed as Hydra jobs; local builds are not a claim of a remote Hydra run.

### Measured conversion result

On a NanoKVM-PCIe at its existing 850 MHz setting, 100 conversions of
1920×1080 UYVY to NV12 using the bridge's production conversion function gave:

| GCC scheduling model | Process CPU time, three runs |
| --- | --- |
| Original generic build | 6.390649, 6.389327, 6.387432 s |
| `thead-c906` | 2.637388, 2.640458, 2.640306 s |

All runs produced checksum `60633ec7`. Allocation, input initialization and
checksumming are outside the timed region. This is approximately 2.42× for
this converter, measured before applying platform-wide tuning. It is **not**
a measured speedup for the whole closure or the hardware video pipeline.
QEMU checks correctness and baseline ISA compatibility, not performance.

The platform-wide tuned RAM image subsequently booted on a LicheeRV camera
with no failed services or initrd unpack errors. The same converter took
6.331373, 6.371392 and 6.354925 CPU seconds in the generic image versus
2.636391, 2.629743 and 2.614910 seconds in the tuned image; all checksums
matched. A separate SHA-256 workload (eight reads of an 8 MiB RAM file)
was essentially unchanged: mean task CPU time over three runs was 5.201 s
generic versus 5.176 s tuned. The benefit is workload-dependent.

Build the current platform benchmark with:

```sh
nix build .#nixosConfigurations.pcie-mainline-sd.pkgs.sg2002-h264-bridge.benchmark
```

For an untuned comparison, evaluate the same board with
`nixpkgs.hostPlatform = lib.mkForce "riscv64-linux"`, then build its
`pkgs.sg2002-h264-bridge.benchmark`. Run `bin/bench-convert` on the physical
board, with competing workloads stopped, alternating the two builds.

## Startup and video

`extlinux-try-boot-bless` waits for the full configured health window only
when the running generation is the recorded trial candidate. Ordinary and
fallback boots skip that window. Malformed state fails closed; actual trial
boots retain their rollback and health checks.

The RAM-only image starts Wi-Fi association when `wlan0` appears, rather
than making the initrd target wait for a missing optional device. On a
LicheeRV diagnostic image with no detected WLAN, userspace startup changed
from 92.218 s to 17.258 s; the latter reached `initrd.target` at 11.908 s.
These are individual boots, not averages. USB upload and firmware time are
excluded. The later boot also exercised experimental SD clock assignments;
its journal confirms the previous 90-second WLAN-device wait was absent.

The initrd uses a feature-selected `systemdMinimal` build with networking,
DNS, NSS, OpenSSL, seccomp, ACLs, journal compression, hardware rules and
console diagnostics retained. On the same RAM-only camera diagnostic,
root-filesystem usage fell from 73,524 to 66,860 KiB, leaving 17,352 KiB free.
This is an initrd-only choice; persistent stage 2 keeps its full systemd.

The tuned PicoClaw RAM image also booted with Wi-Fi, DRM/fbdev and C906L
enabled: userspace startup was 16.145 s, with `initrd.target` at 13.413 s.
It retained 9,268 KiB of root-filesystem space, without extra benchmark
tools. Wi-Fi credentials were supplied at runtime over USB SSH;
association, DHCP and SSH over Wi-Fi then worked. Both initrd and stage-2
supplicant units create the client socket directory expected by `wpa_cli`.

Wi-Fi configurations enable NixOS's `hardware.wirelessRegulatoryDatabase`
even though the large default firmware collection is omitted. The pruned
initrd also retains the SHA-256 crypto module, which cfg80211 needs at
runtime to verify the signed database. On the PicoClaw, database reload
failed with `ENOENT` without the files and `ENODATA` with the signed files
but without SHA-256; adding both made reload succeed while Wi-Fi remained
associated. The country and the driver's self-managed PHY rules were not
changed. The QEMU boot check exercises signed database reload and country
selection in a VM with no radio. Signature verification stays enabled.
This fixes database availability, not the AIC driver's independent
self-managed regulatory policy, and is not a throughput claim.

The 496-byte C906L round-trip test remained approximately 2.50 ms at the
median and 2.59 ms at the 99th percentile over 300 requests. A 16-frame DRM
test completed and restored the display while Wi-Fi carried traffic. This
establishes software completion and coexistence, not optical verification
or a measured framebuffer speedup.

The PCIe composition explicitly includes and loads `sg2002-vpss`. On the
tested running PCIe system, its absence had caused the video service to
restart repeatedly. Loading it created the scaler node and allowed a finite
60-frame capture → VPSS → encoder test to finish successfully. Persistent
cold-boot validation of the module-loading configuration is still required.

Normal U-Boot builds disable MMC tracing and use log level 4. The
`debug = true` package override retains the verbose diagnostic build.
SD kernel command lines no longer force every debug message onto the serial
console. Early console support and the PicoClaw's framebuffer log remain.

## Clocks and IO: current limits

The measured PCIe baseline used a 25 MHz, four-bit SD bus and achieved
11.7 MB/s for a 16 MiB direct read. Its USB link negotiated full-speed
(12 Mbit/s). These are observations, not guaranteed performance figures.

On the tuned PicoClaw, separate 15-second TCP tests received 18.5 Mbit/s
at the board and 41.5 Mbit/s at the host. The receive test had 156 sender
retransmissions; the transmit test had none. A 30-second bidirectional test
while exercising the LCD received 1.74 Mbit/s at the board and 37.7 Mbit/s
at the host. These are individual 2.4 GHz network measurements, not a
before/after optimization comparison. Do not attribute the asymmetry to
the LCD or the CPU compiler setting without further controlled tests.

A later 5 GHz comparison kept the same access point, channel and test tool,
and changed only runtime Wi-Fi power saving between 15-second bidirectional
tests. The initial setting was restored after each test:

| Power saving | Received by board | Received by host | Mean pre-test ping RTT |
| --- | --- | --- | --- |
| On, first run | 3.48 Mbit/s | 101 Mbit/s | 1.357 ms |
| Off | 8.38 Mbit/s | 85.1 Mbit/s | 1.414 ms |
| On, repeated | 10.6 Mbit/s | 78.0 Mbit/s | 1.363 ms |

These short runs do not establish a repeatable benefit from disabling power
saving. No default changed. Read-only MMC diagnostics on the PicoClaw showed
an actual 50 MHz, four-bit SDIO link; the earlier 25 MHz SD-card observation
on another carrier must not be assumed to describe its Wi-Fi connection.

Separate 15-second transfers on that 5 GHz connection, with power saving
restored, received 21.6 Mbit/s at the board (189 sender retransmissions) and
109 Mbit/s at the host (none). CPU-accounting snapshots bracketing those
tests were approximately 67% and 56% idle respectively. The receive-side
limitation therefore remains under investigation; these measurements do
not support treating it as simple CPU saturation or fixing it by overclocking.

A follow-up receive comparison on the same 5 GHz PicoClaw connection kept
power saving enabled and used 20-second measurements after a two-second
warm-up:

| Traffic toward the board | Received throughput | Loss indication |
| --- | --- | --- |
| One TCP stream | 21.6 Mbit/s | 163 sender retransmissions |
| Four TCP streams | 40.4 Mbit/s combined | 1,369 sender retransmissions |
| UDP, offered at 30 Mbit/s with 1,200-byte datagrams | 30.0 Mbit/s | 0 of 62,506 datagrams lost |

Whole-CPU idle time across the respective server runs was approximately
66%, 49% and 29%. These results rule out a fixed 22 Mbit/s receive ceiling;
they do not establish the cause of TCP's lower throughput. Driver ACK
filtering, receive reordering and the network path require controlled
comparisons before any defaults change. All tests completed, and the
watchdog, Wi-Fi and C906L remained healthy afterward.

The CV18xx bypass-mux driver now programs the selected PLL mux as well as
the bypass bit. A RAM-only camera experiment using standard assigned clocks
read back `0x00040009` for both SD clock registers and reported 375 MHz card
clocks with 300 MHz bus clocks. No SD card or SDIO Wi-Fi device was detected
on that test board, so this does not establish media stability or throughput.
The experimental clock assignments and 50 MHz SD limit are not public defaults.

An opt-in high-speed DT on the camera negotiated 480 Mbit/s and sustained
30 seconds of simultaneous TCP traffic: 97.7 Mbit/s received by the board
and 61.7 Mbit/s received by the host, with no sender retransmissions. The
test used a host-connectivity watchdog and a known-good ROM/RAM fallback.
A second camera run with the tuned closure and the unwind-library fix
completed 120 seconds of bidirectional TCP traffic at 97.4 Mbit/s received
by the board and 61.8 Mbit/s received by the host, again with zero sender
retransmissions. SSH, the watchdog and service health checks passed afterward;
the board was then returned to its known-good full-speed RAM image.
This does not validate other carriers, cables or long-term operation. The
existing full-speed default remains, especially given previous PicoClaw
high-speed failures; the separate `sg2002-dtb-mainline-*-high-speed`
packages remain opt-in diagnostics. Likewise, the
CPU's 850 MHz normal setting is unchanged: the vendor higher-frequency mode
also changes core voltage, and is not a safe device-tree-only optimization.

### CPU frequency and voltage scaling

Live clock-framework readback on the camera and PicoClaw reports 850 MHz
for the Linux C906 and 594 MHz for the auxiliary C906. The current kernel
does not enable CPUFreq, and the CPU node has no operating-point table or
voltage-supply connection. Temperature sensing works, but there is no CPU
DVFS policy or frequency-based thermal cooling device. Enabling a governor
alone would not supply that missing hardware integration.

The vendor RISC-V overdrive path sets the main CPU to 1,050 MHz, requests
1.00 V through PWM and changes several other clocks. It is not a validated
1 GHz CPU-only operating point for these board configurations. A future
DVFS implementation needs board-specific supply information, safe clock
transitions and validated operating points, including the effects of the
shared core supply on other engines. No clock or voltage changes were made
as part of these read-only checks.

## Profiling and recovery

`sg2002-kernel-mainline.override { profiling = true; }` enables perf events
and pressure metrics. Preserve the audio/Bluetooth arguments when selecting
this kernel for a board. External Wi-Fi modules follow the selected
`boot.kernelPackages.kernel`, including diagnostic overrides.

The profiling kernel booted on physical SG2002 hardware and exposed the SBI
PMU and `/proc/pressure/{cpu,memory,io}`. Software events (task CPU time,
context switches and page faults) work. Hardware cycle events returned
`ENOENT`, while tested raw events were not counted; PMU enumeration alone
does not establish usable hardware counters. The tuned RAM image and the
two workloads above have been tested; persistent-system and peripheral
performance are separate validation tasks. Do not include
the full `perf` closure in the tiny initrd without checking its actual
filesystem footprint and a clean boot; raw archive size alone can miss
initrd filesystem exhaustion. The QEMU boot check rejects unpacking errors
and requires at least 8 MiB of free root-filesystem space after startup.

The initrd also includes glibc's runtime unwind library explicitly. ELF
dependency pruning cannot discover this `dlopen` dependency; without it,
`pthread_cancel` can abort even when a program starts normally. A physical
PicoClaw reproducer demonstrated the failure and the library's effect, and
the QEMU boot check exercises cancellation, cleanup and thread joining in
the actual initrd. This adds the small library, not the compiler closure.

Mainline images set `watchdog.stop_on_reboot=0`. `nowayout=1` alone does not
protect a kexec handoff: the DesignWare driver's reboot notifier otherwise
stops the counter. Check the running parameter and watchdog state before
testing a new image, and retain an independent recovery path. A still-powered
halted board may reset when the retained watchdog expires. This policy is
not proof that kexec itself works; use the validated ROM-to-RAM upload path
for the current diagnostics. No SD layout change is needed for these tests.

The hardware watchdog reset and ROM-to-RAM recovery were exercised on the
PicoClaw, followed by a normal reboot with the retained-watchdog policy.
The persistent images have been built but not flashed in this optimization
sweep; SD throughput and PCIe cold-boot regression testing remain unverified.

Relevant regression checks:

```sh
nix build \
  .#checks.x86_64-linux.sg2002-c906-tuning \
  .#checks.x86_64-linux.sg2002-h264-bridge-c906 \
  .#checks.x86_64-linux.extlinux-try-boot \
  .#checks.x86_64-linux.sg2002-initrd-eval \
  .#checks.x86_64-linux.sg2002-initrd-boot
```
