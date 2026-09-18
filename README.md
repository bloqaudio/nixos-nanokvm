# nixos-nanokvm

Reusable SG2002 / Sipeed NanoKVM hardware support and a standalone USB-booted
NixOS initrd. The public SG2002 images run entirely in RAM: **no stage 2,
SD card, NFS, NBD, or exported Nix store**.

## USB boot

Build on x86_64 Linux with Nix, supplying an authorized **public** SSH key:

```sh
NANOKVM_AUTHORIZED_KEYS="$HOME/.ssh/id_ed25519.pub" \
  nix build --impure .#boards.picoclaw.mainline.initrd.default.bundle
tar -C result -czf nanokvm-usb.tar.gz .
```

Choose `picoclaw`, `licheerv` or `pcie` for the carrier. The bundle includes
FIP, kernel/initrd FIT, uploader and a Dockerfile. The upload machine needs
USB access, Python and fastboot, but **does not need Nix**. Uploading is
RAM-only, never flashing storage. The uploader exits after handoff.

The image includes key-only SSH, DHCP and link-local networking on USB,
Ethernet and Wi-Fi, DNS, ALSA audio tools, and a hardware watchdog. PicoClaw
also includes the C906L Rust firmware, DRM/`/dev/fb0` display path and
C906L-mediated Wi-Fi power control. Carrier wiring determines which devices
are usable; build checks do not substitute for live peripheral tests.

See [the standalone guide](docs/usb-initrd.md) for Wi-Fi credentials, Linux
and Docker upload instructions, ROM reset, USB SSH discovery and diagnostics.
Native Windows/macOS upload is not yet supported; a Linux VM needs explicit
USB passthrough. Docker Desktop does not supply that automatically.

Additional outputs at the same attrpath: `fit`, `kernel`, `initrd`,
`usb-boot`. No unauthenticated network shell or default password is provided.
Wi-Fi credentials, if embedded, are readable in the Nix store and bundle:
never publish those images to a public cache.

## Hardware development

This repo retains the mainline/vendor kernels, U-Boot/FIP/DTB builders,
AIC8800 Wi-Fi support, audio, camera/ISP/codec drivers, C906L toolchain and
firmware, and reusable board modules. The NanoKVM web application is packaged
separately; it is not run by the minimal initrd.

```sh
nix develop .#c906l
nix build .#checks.x86_64-linux.sg2002-c906l-rust-all-timers
nix build .#checks.x86_64-linux.sg2002-initrd-eval
nix build .#nanokvm-server
```

The `licheerv.mainline.initrd.c906l-all-timers` target provides the opt-in
Timer4–7 firmware. Camera/ISP blocks remain Linux-owned, not C906L firmware.
See [C906L architecture and hardware evidence](docs/sg2002-c906l.md) and
[mainline ISP support](docs/sg2002-mainline-isp.md). Commands in historical
network-root bring-up reports describe an older development workflow.

The separate `qemu-c906-virt` app runs a full development VM with the shared
kernel and a 256 MiB C906 model. QEMU has no SG2002 peripheral model: it cannot
validate LCD, SDIO Wi-Fi, USB gadget, audio or camera hardware.

## Downstream integration

SD and network-root deployment policy is not shipped by this repository.
The former SG2002 `live`, `debug`, `kernel-test` and `sd` outputs are retired;
use the standalone initrd targets for USB boot. Projects needing persistent
or network-root systems can compose the reusable hardware modules with their
own deployment profiles and host services.

Reusable `boards/`, `platform/` and hardware `modules/` remain here.
`nixosModules.boards.<board>.mainline.initrd.default` is the new importable
RAM-only composition; stage-2 consumers should compose hardware modules with
their own profile, not layer a root filesystem onto it. `nixosModules.default` still exposes the
NanoKVM service and package overlay for other consuming configurations.
SpacemiT K3 outputs are unchanged by this SG2002 separation.

## Layout and checks

| Path | Purpose |
| --- | --- |
| `boards/`, `platform/` | Carrier and SoC hardware definitions |
| `profiles/usb-initrd.nix` | RAM-only SSH/network/peripheral environment |
| `lib/catalog.nix` | Public SG2002 initrd targets |
| `lib/initrd-artifacts.nix` | Bounded FIT, upload runner and portable bundle |
| `pkgs/` | Kernels, drivers, firmware, toolchains and application packages |
| `tests/` | Module, firmware, DT and uploader regression checks |

`hydraJobs.x86_64-linux` builds all catalog images, the uploader regression
tests, the 256 MiB boot test, and hardware checks. CI images are deliberately
locked: they contain no authorized SSH keys or Wi-Fi credentials. Build your
own keyed bundle as above; its kernel, firmware and tools can reuse CI's cache.

For application patches, update `patches/nanokvm/`; kernel and bootloader
patches live under `pkgs/sg2002/`. Keep reusable hardware fixes here and
machine-specific policy in the consuming flake.
