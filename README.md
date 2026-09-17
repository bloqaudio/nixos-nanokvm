# nixos-nanokvm

Reusable NixOS board support for Sipeed NanoKVM / SG2002 hardware
(Sophgo CV1800 family, RISC-V C906).

This repository owns the hardware layer:

- SG2002/NanoKVM board modules and profiles
- mainline and vendor kernel packaging
- mainline U-Boot/FIP/DTB plumbing
- AIC8800 SDIO WiFi driver and firmware packaging
- NanoKVM userspace packaging
- USB boot, kexec, and NBD live-image tooling

It intentionally does not own personal deployment policy. Keep hostnames,
LAN addresses, routes, DNS, WiFi credentials, SSH users/keys, secrets, and
Colmena topology in the downstream flake that consumes this one.

## Current Target

The primary NanoKVM-PCIe path is mainline:

- `boards.pcie.mainline.sd`: SD image using mainline U-Boot/extlinux and
  Linux 7.2-rc5
- `boards.picoclaw.mainline.sd.c906l-lcd`: SD image with Linux DRM
  scanout through the C906L-owned PicoClaw LCD, plus AIC8800 Wi-Fi
- Ethernet via `stmmac`
- AIC8800 SDIO WiFi via the Radxa driver plus local SDIO compatibility
  patching
- USB gadget networking for recovery/control
- OLED and NanoKVM userspace support

Vendor-kernel outputs remain available for recovery and regression testing,
but new reusable work should prefer the mainline profile unless there is a
specific vendor-only dependency.

## Build

From this repository on an x86_64 Linux host:

```sh
nix build .#boards.pcie.mainline.sd
nix build .#boards.picoclaw.mainline.sd.c906l-lcd
nix build .#nanokvm-server
```

Useful development artifacts:

```sh
nix run .#boards.licheerv.mainline.kernel-test.usb-boot
nix run .#boards.licheerv.mainline.live.usb.usb-boot
nix run .#boards.licheerv.mainline.live.usb.kexec
nix run .#boards.picoclaw.mainline.kernel-test.usb-boot
nix run .#boards.picoclaw.mainline.live.usb.usb-boot
```

The USB live runner configures the host side of the ECM link as
`10.55.0.2/24`, serves the root filesystem over NBD, and passes the selected
endpoint on the kernel command line. The target comes up at `10.55.0.1/24`.

For a WiFi-backed live rootfs, provide both a target-side WiFi configuration
through a consuming NixOS config and the site-specific host address at runtime:

```sh
NANOKVM_NBD_ROOTFS_HOST=192.0.2.10 \
NANOKVM_NBD_ROOTFS_BIND=192.0.2.10 \
  nix run .#boards.licheerv.mainline.live.wifi.usb-boot
```

`NANOKVM_NBD_ROOTFS_HOST` is the address the target can reach over WiFi.
`NANOKVM_NBD_ROOTFS_BIND` is optional; omit it to let `nbd-server` bind all
local interfaces.

The PicoClaw live outputs use the diskless Strix pattern instead: `/` is
tmpfs, `/nix/store` is Trex's read-only NFSv4 export with a local tmpfs
overlay, and no writable state is exported. The plain variant mounts over
the static USB gadget link; the WiFi variant avoids the SG2002 mainline
DWC2 RX stall by using WLAN for NFS while retaining USB for ROM download,
fastboot, and ACM diagnostics:

```sh
NANOKVM_WIFI_CONFIG=$PWD/wifi.conf \
  nix run --impure .#boards.picoclaw.mainline.live.wifi.usb-boot
```

The initrd needs the WiFi credential before it can mount the store, so this
necessarily places the configuration in the Nix store and FIT image. Use a
dedicated development SSID or PSK rather than a broadly privileged credential.

## Mainline camera ISP

The camera's hardware ISP is Linux-owned; it does not require firmware on the
auxiliary RISC-V core. The opt-in `usb-cam-isp` RAM image exposes Bayer-to-NV21
capture and a DMA-BUF path through VPSS into the H.264 encoder. The initial
fixed-settings implementation completed 300 live frames at about 29.5 fps;
lit-scene colour validation and automatic image tuning remain outstanding.
See [the hardware ISP guide and board evidence](docs/sg2002-mainline-isp.md).

## C906L Rust firmware

The auxiliary 700 MHz RISC-V core has opt-in `no_std` Rust firmware, a
reproducible bare-metal toolchain, Linux mailbox/RPMsg integration and typed
Timer4–7 drivers. Firmware, reserved memory, device tree and Linux drivers
share a generated contract. Camera and ISP hardware remains Linux-owned.

```sh
nix develop .#c906l
nix build .#checks.x86_64-linux.sg2002-c906l-rust-all-timers
nix build .#checks.x86_64-linux.sg2002-c906l-firmware-all-timers
nix run .#boards.licheerv.mainline.live.usb-c906l.usb-boot
```

The last command boots an opt-in RAM image on an attached board. See
[the C906L guide](docs/sg2002-c906l.md) for peripheral selection, recovery and
the distinction between build verification and completed hardware tests.

## QEMU C906 Sandbox

`boards/qemu-riscv-virt.nix` boots the board's own kernel on
`qemu-system-riscv64 -M virt -cpu thead-c906 -m 256 -smp 1`, with a
visible virtio-gpu console:

```sh
nix run .#qemu-c906-virt
QEMU_OPTS='-display vnc=:0' nix run .#qemu-c906-virt   # headless host
```

`-m 256` and `-smp 1` come from the mainline DT, not from taste:
`sg2002.dtsi` declares `memory@80000000 reg = <0x80000000 0x10000000>`, and
`cv180x-cpus.dtsi` declares a single `cpu@0` with `compatible =
"thead,c906"` and `riscv,isa = "rv64imafdc"`. The die's second C906 (700
MHz) and its 8051 are separate firmware domains, not SMP siblings, so
Linux never enumerates them.

The kernel is `pkgs.sg2002-kernel-mainline` — the same store path the
boards get, verifiable with:

```sh
nix eval --raw .#nixosConfigurations.qemu-c906-virt.config.boot.kernelPackages.kernel
nix eval --raw .#nixosConfigurations.picoclaw-mainline-live-usb-lcd.config.boot.kernelPackages.kernel
```

To make that possible, `linux-mainline/config.nix` keeps PCI, virtio, 9p
and DRM — none of which exist on the SG2002 — modular wherever Kconfig
allows. The shared kernel also enables IPv6 and the netfilter support
needed by the guest's NixOS firewall. Built-in support increases the board
kernel's footprint; the QEMU-specific modules are loaded only in the guest.
See the "PCIe / virtio / DRM" and networking blocks in that configuration.

QEMU has no CV181x machine model, so nothing SoC-specific runs here: no
SPI (no ST7789), no I2C (no SSD1307), no CSI, no USB gadget, no Coda980.
This is for userspace, systemd units, and C906 codegen — not peripherals.

## Downstream Use

Import the reusable board module, then add deployment-specific policy in your
own NixOS configuration:

```nix
{
  inputs.nanokvm.url = "github:georgewhewell/nixos-nanokvm";

  outputs = { nixpkgs, nanokvm, ... }: {
    nixosConfigurations.nanokvm = nixpkgs.lib.nixosSystem {
      system = "riscv64-linux";
      modules = [
        nanokvm.nixosModules.boards.pcie.mainline.sd
        ({ ... }: {
          networking.hostName = "nanokvm";
          services.openssh.enable = true;

          # Deployment-specific choices belong here, not in this repo.
          sg2002.wifi.enable = true;
        })
      ];
    };
  };
}
```

For non-SG2002 development hosts, `nanokvm.nixosModules.default` exposes the
portable NanoKVM service and package overlay. Disable cv181x-only hardware
features in the consuming configuration:

```nix
{
  imports = [ nanokvm.nixosModules.default ];

  services.nanokvm = {
    enable = true;
    kmods.enable = false;
    usbGadget.enable = false;
    hdmi.enable = false;
    httpPort = 8080;
    httpsPort = 8443;
    openFirewall = true;
    hardwareVersion = "pcie";
  };
}
```

## Local Files

These files are intentionally ignored and only affect local standalone builds:

- `authorized_keys`: root SSH public keys baked into local images; Git-flake
  builds instead use `NANOKVM_AUTHORIZED_KEYS=/absolute/path/authorized_keys`
  together with `--impure`
- `wifi.conf`: local `wpa_supplicant` configuration, injected into a standalone
  WiFi image with `NANOKVM_WIFI_CONFIG=$PWD/wifi.conf` and `--impure`
- `.ssh_host_*_key`: cached per-developer SSH host keys injected by USB boot
- `.nanokvm-*.log` / `.nanokvm-*.pid`: host runner state
- `media/captures/`: local terminal recordings and rendered GIFs

Use a downstream secrets system for WiFi credentials.

The PicoClaw C906L SD image requires an explicit SSH public key and supports
either pre-baked or USB-first Wi-Fi provisioning.  See [its build, write and
credential instructions](docs/sg2002-c906l-picoclaw-sd-image.md).

## Patch Workflow

For local fixes to upstream NanoKVM userspace:

```sh
git -C ../NanoKVM diff > patches/nanokvm/0001-my-change.patch
nix build .#nanokvm-server
```

Kernel and bootloader patches live under `pkgs/sg2002/linux-mainline/` and
`pkgs/sg2002/uboot-mainline/`. Keep reusable hardware fixes here; keep
machine-specific configuration in the consuming NixOS flake.

## Layout

| Path | Purpose |
| --- | --- |
| `boards/` | Per-board NixOS modules |
| `platform/` | Shared CV1800/SG2002 platform module |
| `profiles/` | SD, live, debug, and kernel-test profiles |
| `modules/` | Reusable NixOS modules for services and boot plumbing |
| `lib/catalog.nix` | Single source of truth for shipped board outputs |
| `lib/artifacts.nix` | Host-side artifact and runner builders |
| `lib/protocol.nix` | USB-ECM MACs, IPs, and ports |
| `pkgs/` | Overlay packages, kernels, firmware, U-Boot, FIP, DTBs |
| `scripts/` | Host-side development utilities |

## Continuous integration and binary cache

Pushes to `master` and manual **Build and cache** runs use the `ax102-nanokvm`
self-hosted runner. CI builds `nanokvm-server`, the PCIe mainline SD image,
the PicoClaw C906L LCD SD image, and the checks listed in `scripts/ci-build.sh`.
The runner uses pure evaluation without developer SSH keys or Wi-Fi secrets.
The PicoClaw image supports root login with password `nixos-nanokvm`; change it
with `passwd` after boot.

ax102 imports successful build closures from the runner VM and publishes them
through Harmonia. A green workflow includes verification that its output
paths are available at `https://cache.hellas.ai`. Imported outputs are rooted
for 30 days; the latest successful run stays rooted. To consume them, add:

```nix
nix.settings = {
  extra-substituters = [ "https://cache.hellas.ai" ];
  extra-trusted-public-keys = [
    "cache.hellas.ai-1:PYolh95U/Ms5fKE+NQTcNZUHyEv4QikaNocg9I9iy0g="
  ];
};
```
