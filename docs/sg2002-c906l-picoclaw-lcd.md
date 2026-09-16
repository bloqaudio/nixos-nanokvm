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
and production Linux transport functions. Hardware evidence will be recorded
separately; passing these tests alone does not establish visible LCD output.
