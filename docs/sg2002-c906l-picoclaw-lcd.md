# PicoClaw LCD through C906L and DRM

This is a dedicated, reversible PicoClaw experiment. It is not a camera-board
profile and must not be deployed on a NanoKVM carrier with Ethernet in use.

## Architecture

Linux applications use standard DRM/KMS dumb buffers and page flips. Linux
renders XRGB8888 pixels into GEM shared-memory objects, converts a complete
240x240 frame into RGB565 big-endian bytes, and publishes it into one of two
reserved DDR slots. C906L's Rust service reads that immutable slot and drives
the ST7789 through SPI1. Pixel data does not travel in RPMsg messages.

```text
Linux application -> DRM/KMS GEM buffer -> reserved RGB565 slot
                                              |
                                   C906L Rust -> SPI1 -> LCD
                                              |
                                completion -> DRM flip event
```

The current design intentionally copies into reserved DDR. It does not expose
those physical slots through a custom mmap or device-file ABI. This prevents
applications from overwriting a buffer while firmware is scanning it out.
DRM's fbdev compatibility layer is used rather than an independent fbdev
driver. The existing mailbox and byte-exact RPMsg echo remain available.

This is a display controller, not a GPU: rendering is software on Linux, while
C906L performs panel initialization and SPI transfers. Scanout completion means
the SPI transfer has finished; without the panel's tearing-effect signal this
does not establish tear-free presentation or a physical vertical-blank event.
The nominal DRM mode must not be interpreted as a guaranteed refresh rate.

## Shared-memory ownership

The generated `picoclaw-lcd` contract retains ABI 1.1 and assigns lease bit 4,
profile ID 17, and final capabilities `0x8b`. Its immutable digest covers the
physical resources, panel geometry, pin preconditions and framebuffer protocol.

| Resource | Address | Size |
| --- | --- | --- |
| Slot 0 ownership pair | `0x8ff50000` | 128 bytes |
| Slot 1 ownership pair | `0x8ff50080` | 128 bytes |
| Slot 0 pixels | `0x8ff51000` | 115,200 bytes |
| Slot 1 pixels | `0x8ff6e000` | 115,200 bytes |

Each pair has a Linux-written request cacheline and a C906L-written completion
cacheline. Requests contain the boot generation, a nonzero per-slot sequence,
exact frame length, reserved zero bytes and a commit word written last.
Completions match that generation and sequence and include an error result.

Linux publishes pixels before the request commit using its write-combined
mapping and write barriers. C906L accepts two identical, explicitly invalidated
request snapshots, invalidates the complete pixel range, and reads the frame
only while it owns the slot. It cleans its completion cacheline and publishes
the completion commit last. Linux cannot reuse the slot until matching success.
Errors retain ownership and fail closed rather than replaying partial transfers.

## Peripheral ownership and recovery

Linux retains clock/reset/pin-routing management, applies the PicoClaw-specific
EPHY-to-SPI pad handoff only after the firmware manifest matches, and authorizes
the static lease. Firmware checks the generated read-only pad prerequisites.
SPI1 and the whole GPIOA register bank then belong exclusively to C906L.

The dedicated DT disables Linux SPI1/spidev, GPIOA, I2C0, Ethernet and the
SDIO/Wi-Fi controller; the SD-card controller remains enabled. This is necessary
because LCD D/C shares I2C0's clock pad and Wi-Fi power uses GPIOA26. It does not silently share GPIO read/modify/
write registers between Linux and firmware. Other firmware GPIO bits are
preserved. The normal U-Boot splash and Linux LCD service are not used.

The Rust panel driver sends at most 32 complete, eight-byte SPI transactions
per service step, with scheduler-backed initialization deadlines. Mailbox and
heartbeat processing stay independent. A terminal panel fault switches the
backlight off and retains the lease until whole-board reset. Runtime unbind,
module unload, suspend and C906L reset are not validated recovery paths.

Both Linux watchdog stages monitor the USB host, and the ROM runner can arm
the reset-only U-Boot watchdog with `--uboot-watchdog`. Use RAM boot only; no
flash operation is needed.

## Build and test

```console
nix build --builders '' --no-link --print-out-paths \
  .#boards.picoclaw.mainline.live.usb-c906l-lcd.usb-boot
```

On the board, verify `sg2002-c906l-ctl check` before display testing. The
`sg2002-c906l-drm-test /dev/dri/card0 4` tool verifies the DRM driver identity,
allocates two standard XRGB8888 dumb buffers, draws changing checkerboards,
waits for page-flip events, holds the final pattern briefly, and restores the
previous display mode. It refuses unrelated graphics devices.

Host tests cover the exact contract, invalid ownership records, frame bounds,
cacheline layout, panel command sequencing, bounded SPI work, terminal errors,
and production Linux transport functions. Passing these tests alone does not
establish visible LCD output.

## Hardware validation: 2026-09-16

The PicoClaw on workstation `fuckup`, USB port `3-4`, RAM-booted the dedicated
Linux 7.2-rc5 image. No flash operation or workstation reboot was performed.
The tested runner was
`/nix/store/8zk8qq4vydy5k8ilmd82a6pm78c0ph73-usb-boot`, built from the implementation
through commit `9e60f77`. The exact LCD contract digest was
`0c2d81d5523800863533be8dc1424b8b8ff5a5665a258674a06bd70a1bdebbb0`.

Observed results:

- Activation completed in one attempt, generation 2, capabilities `0x8b`,
  firmware flags zero. The mailbox identity/ping check passed before and after
  the first display test.
- `/dev/dri/card0` and `/dev/fb0` registered as `sg2002-c906l` /
  `sg2002-c906ldrm`. The normal fbdev console also submitted acknowledged frames.
- Standard dumb-buffer/modeset/page-flip tests passed for 4 frames and then
  32 frames, both exiting zero and restoring the previous display mode.
- Concurrent 496-byte RPMsg tests passed for 1,000 and 10,000 messages. The
  latter measured median 2,497.240 us, p99 2,680.520 us, maximum 5,168.600 us.
  These are observations under this workload, not real-time latency guarantees.
- A direct `LCQ1` diagnostic query returned 101 completed frames, per-slot
  completed sequences 51 and 50, panel state BUSY, zero fault and zero malformed
  request status. The busy state and one outstanding Linux sequence reflect
  continuing fbdev updates, not a lost acknowledgement.
- U-Boot's reset-only watchdog was armed and read back; Linux reported an
  active 85-second watchdog in both initrd and stage 2.
- Recovery was fault-injected by administratively disabling only the host's
  USB network interface at 19:29:10 UTC. Without a software reboot command,
  the board disconnected at 19:31:06 and re-enumerated as `3346:1000` ROM at
  19:31:08. The 116-second reset interval is consistent with six failed
  five-second health probes followed by the hardware watchdog countdown.
  The workstation's normal loader caught the reset.

The first restoration attempt stalled waiting for Wi-Fi and reset again. The
normal loader retried, and the original `claw` image recovered Wi-Fi and SSH at
19:38:34 UTC; its host boot service then exited successfully. The temporary boot
inhibition was removed, and all test-owned NBD/boot processes and the port 12502
listener were absent at final cleanup. The workstation was not rebooted. SSH
authentication was unavailable to this session, so the original image's stage-2
LCD service was not independently rechecked after restoration.

These measurements establish acknowledged SPI transfers, shared-memory reuse,
and simultaneous IPC on the real board. They do not independently establish
correct visible colours/orientation or tear-free presentation. The user was
not near the board, so visual confirmation remains pending.

Two cold-boot findings were fixed before the successful test: the over-strict
EPHY comparison described below, and the NBD initramfs accidentally depending
on optional kexec packaging for its real client. The no-kexec image now includes
that executable and its runtime libraries explicitly and calls it by absolute
path, preventing fallback to BusyBox's incompatible applet.

Console, loader and recovery logs are retained on the development host in
`/mnt/Home/src/nixos-nanokvm-picoclaw-evidence-20260916.mmhC01`.

## EPHY handoff register validation

The primary register-field reference is SOPHGO's
[SG2002 PINOUT workbook at commit `12d2bc6976400e6d40389f3faaff40f4326b63c2`](https://github.com/sophgo/sophgo-hardware/blob/12d2bc6976400e6d40389f3faaff40f4326b63c2/SG200X/04_SG2002/04_SG2002_PINOUT.xlsx),
worksheet `6. 如何把 MIPI Audio ETH 切入GPIO`, cell `B27`.
The downloaded workbook's SHA-256 is
`a20e1d2f02b0350a333ff16538cc59c13372b88c8a96f9737a3ef4f5ff57c148`.

That cell identifies bits `[10:9]` and `[2:1]` at both `0x03009074` and
`0x03009070` as the EPHY pad input/output enables and specifies `0x606`.
Their enable-field readback condition is therefore `(value & 0x606) == 0x606`,
not full-register equality to `0x606`. Linux retains the existing full vendor
write of `0x606`; Linux's readback check and the firmware's activation contract
check the documented enable fields. Page selection, top-level GPIO routing,
power/reset prerequisites and pinmux checks remain separate and unchanged.

The first RAM-boot activation attempt observed `0x1606` at `0x03009074` and
`0x1616` at `0x03009070`, which satisfy those enable fields but failed the
original full-register check. This is not a whitelist of observed values:
clearing any of the four documented enable bits must still reject activation.
The inspected primary sources do not classify bits 4 and 12 as read-only,
status, or writable fields. Their differing values do not establish their
meaning, and no fixed expected value is asserted for them.
