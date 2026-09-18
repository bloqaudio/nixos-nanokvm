# PicoClaw standalone-initrd validation: 2026-09-18

The SG2002 PicoClaw booted Linux 7.2-rc5 and the RAM-only systemd initrd from
ROM download mode. Upload ran inside Debian bookworm with Python, PyUSB,
pyserial and its fastboot 29 package; no Nix installation, exported store,
NFS or NBD service was available to that container. No image was flashed and
the workstation was not rebooted.

## Fixes exercised by the fresh boot

- The CV18xx MMC driver acquires its optional `timer` clock through Linux's
  clock framework. SDIO1 needs this 100 kHz reference alongside its core/bus
  clocks. Deferring probe until the C906L Wi-Fi regulator is available had
  exposed the missing dependency after unused firmware clocks were gated.
  The new device-tree clock and managed driver reference restored automatic
  SDIO enumeration; no diagnostic module or manual rebind was used on this boot.
- The LCD uses normal DRM fbdev emulation and fbcon, with `console=tty0`,
  `fbcon=nodefer` and the built-in MINI4x6 font. Its platform module probes
  through udev, outside the synchronous forced-module list. UART remains the
  primary console. The active virtual terminal contained kernel log text,
  framebuffer-console binding was enabled, and the console reported 60x40.
- The uploader accepts older fastboot's padded memory-dump output and stays
  within its 64-byte command limit. A USB disconnect is reported only as a
  handoff, not as proof Linux started. Actual boot was checked over SSH.
- Missing runtime Wi-Fi credentials skip wpa_supplicant cleanly. Credentials
  were subsequently installed over authenticated SSH, not embedded in this
  tested bundle or published to a cache.

## Combined hardware results

Wi-Fi associated, acquired IPv4/IPv6 addresses and resolved DNS. An SSH
transfer over Wi-Fi delivered exactly 8,388,608 bytes while the display test
ran. USB SSH remained responsive during the tests.

The standard DRM dumb-buffer/modeset/page-flip test completed 32 frames and
restored the previous display. Silent ALSA playback and capture each ran for
two seconds at 48 kHz, stereo S16_LE, using `hw:0,0` and `hw:0,1` respectively.
An overlapping 1,000-message, 496-byte RPMsg round-trip test measured:

| Median | p99 | Maximum |
| --- | --- | --- |
| 2.495 ms | 2.684 ms | 3.393 ms |

Afterward, C906L contract validation passed, activation attempts remained one,
and both LCD and Wi-Fi regulator transports reported zero faults. No systemd
units had failed. The hardware watchdog was active with an 85-second timeout
and `nowayout=1`. Approximately 77 MiB remained available. Mount inspection
showed only the initrd and kernel virtual filesystems, with no stage 2.

Temporary host networking overrides, recovery timer, uploader containers and
the host-side temporary SSH private key were removed. The tested image stayed
running and reachable over Wi-Fi; normal host USB-loader configuration was
restored without starting a replacement boot.

## Boot timing

`systemd-analyze time` reported 30.045 seconds of userspace startup and
`initrd.target` at 29.271 seconds of userspace time. PID 1 began approximately
7.6 seconds after Linux started. These are one-boot observations, not averages.

| Milestone | Approximate time since Linux start |
| --- | ---: |
| Watchdog keeper active | 14.1 s |
| USB gadget service active | 22.1 s |
| SSH listening | 25.8 s |
| Framebuffer text-console takeover | 31.5 s |
| DRM fb0 registration complete | 33.8 s |
| initrd.target reached | 36.9 s |

The largest `systemd-analyze blame` entries were udev trigger (10.104 s),
forced module loading (5.216 s), and DNS service startup (2.597 s). These
durations overlap and must not be added together. The earlier failing-Wi-Fi
boot reached the initrd target at approximately 99.8 seconds since Linux start.

ROM/firmware USB upload took approximately 48 seconds separately. Neither
that transfer nor U-Boot execution is included in the userspace timing report.
This board uses U-Boot, not systemd-boot; the timing tool is `systemd-analyze`.

## Identity and limits

- Tested FIT: 36,690,232 bytes, SHA-256
  `c2b0496b54d5ef81993037079e19c1931c63275b0f932a9876f13d9cfb46c4ec`.
- C906L ABI 1.1, combined `picoclaw-lcd` contract:
  `2ff551e54e51c569cc0539a4ab93e47438666288b861e248a5c77cc53ab92fd2`.
- The 256 MiB QEMU initrd boot test, module/DT checks and all 29 uploader unit
  tests passed. FIT builds enforce the compressed and expanded size limits.

There was no person inspecting the panel or listening to the speaker. DRM
completion and console state establish the software/transport result, not a
human visual inspection; silent playback/capture do not establish acoustic
quality. The LCD is a read-only boot-log console, not an unauthenticated getty.
It cannot show ROM/U-Boot or the first Linux messages live before its validated
firmware transport is ready. No display-failure fault injection was performed
on this final image; its optionality is established by configuration checks.

This test does not validate SD boot. The reported older Btrfs/extlinux failure
(`zstd_decompress: failed to decompress: 70`) is a separate boot-file read
problem; the USB FIT/initrd path does not read that filesystem.
