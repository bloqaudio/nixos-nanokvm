# A C906 sandbox: `qemu-system-riscv64 -M virt -cpu thead-c906 -m 256 -smp 1`.
#
# What this is
# ------------
# This sandbox uses QEMU's generic RISC-V virt machine, not an SG2002
# peripheral model. It is deliberately
# *not* a board file in the sense of boards/licheerv-nano-*.nix: it does
# not import platform/cv181x.nix, does not set `sg2002.enable`, and builds
# no FIP/FIT/U-Boot. Nothing here ever runs on real silicon.
#
# What it does match, exactly, is the CPU and the memory budget that
# arch/riscv/boot/dts/sophgo/{cv180x-cpus,sg2002}.dtsi describe:
#
#   cpu@0  compatible = "thead,c906"      ->  -cpu thead-c906
#          riscv,isa  = "rv64imafdc"          (no V: QEMU's thead-c906
#          mmu-type   = "riscv,sv39"           has no vector either)
#   memory@80000000 reg = <0x80000000 0x10000000>  ->  -m 256
#
# and one core. The SG2002 die does carry a second C906 (700 MHz) and an
# 8051, but they are separate domains running their own firmware out of
# carved-out DRAM, not SMP siblings — the mainline DT has a single cpu
# node and Linux only ever enumerates one. Hence `-smp 1`.
#
# The kernel is `pkgs.sg2002-kernel-mainline` — byte-identical to the one
# the boards boot. This exercises shared kernel and userspace code, but
# does not reproduce the board's peripheral or timing behaviour. The three
# subsystems QEMU needs and the SG2002 does not (PCI, virtio, DRM) live
# in the shared config.nix, modular, under the "PCIe / virtio / DRM"
# heading; the board builds them and never loads them.
#
# What it is good for: userspace, systemd units, the RISC-V toolchain,
# C906-specific codegen, and anything that does not touch SoC registers.
# What it cannot do: SPI, so no ST7789; I2C, so no OLED; CSI, USB gadget,
# Coda980. Those need the real board.
#
# Usage:
#   nix run .#qemu-c906-virt
#   QEMU_OPTS='-display vnc=:0' nix run .#qemu-c906-virt   # headless host
{
  config,
  lib,
  pkgs,
  modulesPath,
  allowUnfreePredicate,
  selfOverlay,
  rootAuthorizedKeys ? [ ],
  ...
}:
{
  imports = [ (modulesPath + "/virtualisation/qemu-vm.nix") ];

  nixpkgs.hostPlatform = "riscv64-linux";
  nixpkgs.buildPlatform = "x86_64-linux";
  nixpkgs.config.allowUnfreePredicate = allowUnfreePredicate;
  nixpkgs.overlays = lib.mkAfter [ selfOverlay ];

  boot.kernelPackages = pkgs.linuxPackagesFor pkgs.sg2002-kernel-mainline;

  # QEMU direct-boots the Image via `-kernel`; the board's USB-recovery
  # path loads a FIT through U-Boot. Left at
  # its NixOS default, grub would try to cross-build its perl
  # (XML::LibXML) installer helper for riscv64, which does not build.
  boot.loader.grub.enable = false;
  boot.loader.generic-extlinux-compatible.enable = false;

  # virt is a device-tree machine like the SG2002, so the same
  # earlycon-then-handoff console story applies; QEMU's UART is a plain
  # 16550 at 0x10000000.
  boot.kernelParams = [ "earlycon=uart8250,mmio,0x10000000" ];

  # A pruned initrd module list, for the same reason the boards keep one
  # behind `sg2002.initrd.pruneKernelModules`: modules-closure resolves
  # every name against the real module tree and dies on the first miss,
  # and this kernel is far smaller than a stock NixOS one. Two upstream
  # defaults would otherwise land here and fail —
  #   * the PC storage/HID list (ahci, sata_nv, ata_piix, nvme, sd_mod,
  #     usbhid), against `SCSI = no; ATA = no; HID = no`;
  #   * efivarfs, which systemd stage 1 adds whenever systemd is built
  #     withEfi, against `EFIVAR_FS = no`.
  # mkForce rather than includeDefaultModules = false, so a future
  # upstream addition fails visibly here instead of at boot. autofs is
  # systemd's; 9p/9pnet_virtio come from the store mount's fsType.
  boot.initrd.includeDefaultModules = false;
  boot.initrd.availableKernelModules = lib.mkForce [
    "autofs"
    "ext4"
    "9p"
    "9pnet_virtio"
    "virtio_pci"
    "virtio_blk"
    "virtio_net"
    "virtio_gpu"
    "drm"
  ];

  # virtio_gpu is what lights the window up: until it loads, tty0 is bound
  # to dummycon and the display stays black — which is exactly the window
  # in which you most want to see what stage 1 is doing.
  boot.initrd.kernelModules = [ "virtio_pci" "virtio_gpu" ];

  # The runner script, qemu itself, and the closure/image helpers all
  # execute on the build host, not in the guest. Without this the module
  # would try to hand us a riscv64 qemu and a riscv64 bash to run it with.
  # `buildPackages` of the cross set is exactly the x86_64-linux native
  # set. qemu-vm then picks `qemu` (all targets) over `qemu_kvm`
  # (host-arch only) on its own, because the qemuArch values differ.
  virtualisation.host.pkgs = pkgs.buildPackages;

  virtualisation = {
    memorySize = 256; # sg2002.dtsi: 0x10000000
    cores = 1; # cv180x-cpus.dtsi: one cpu node
    diskSize = 2048;
    graphics = true;

    # The store comes in read-only over 9p with a writable overlay, so a
    # rebuild is a rebuild of the closure and not of a disk image. TCG is
    # slow enough that this matters a great deal.
    writableStore = true;

    # Nothing here orchestrates the guest from the host, and leaving this
    # on cross-builds the whole of QEMU for riscv64 just to get
    # qemu-ga into the closure.
    qemu.guestAgent.enable = false;
  };

  virtualisation.qemu.options = [
    # nixos/lib/qemu-common.nix hardcodes `qemu-system-riscv64 -machine
    # virt` for a riscv64 guest and passes no -cpu, which would leave us
    # on the generic `rv64` model (RVA22-ish: vector, sstc, svpbmt — none
    # of which a C906 has). This is the whole point of the exercise.
    "-cpu thead-c906"

    # virt has no VGA at all; the display is a PCIe virtio-gpu. With
    # DRM_FBDEV_EMULATION from the shared config.nix, fbcon paints this
    # window from stage 1 onward — there is no X, no Wayland, no
    # userspace driver involved.
    "-device virtio-gpu-pci,xres=1024,yres=768"
    # The keyboard comes from virtualisation.qemu.virtioKeyboard (on by
    # default, emitted as a bare `-device virtio-keyboard` that QEMU
    # resolves to the PCI variant); only the pointer is missing.
    "-device virtio-tablet-pci"

    # Last -display wins, so QEMU_OPTS='-display vnc=:0' still overrides
    # this when there is no X/Wayland session on the build host.
    "-display gtk,zoom-to-fit=on"
  ];

  # Serial stays wired up alongside the window: ttyS0 gets the printk
  # stream, tty0 (the window) is /dev/console and carries the getty.
  systemd.services."serial-getty@ttyS0".enable = true;

  # The window and the serial console both land in a root shell without
  # a password prompt; sshd stays key-only, exactly as on the boards.
  services.getty.autologinUser = lib.mkDefault "root";
  users.users.root.openssh.authorizedKeys.keys = rootAuthorizedKeys;

  services.openssh = {
    enable = true;
    settings.PermitRootLogin = "prohibit-password";
  };
  virtualisation.forwardPorts = [
    {
      from = "host";
      host.port = 2222;
      guest.port = 22;
    }
  ];

  # nixpkgs builds the networking flags with the legacy
  # `-net nic,netdev=user.0,model=virtio`. The RISC-V virt machine never
  # instantiates those — it has no onboard NIC and does not call QEMU's
  # legacy NIC-creation pass — so QEMU only warns
  #   requested NIC (model virtio) was not created (not supported by this
  #   machine?)
  # and the guest boots with no network interface whatsoever, which makes
  # the ssh forward above silently dead. virt needs an explicit PCIe
  # device. Rebuilt from config.virtualisation.forwardPorts so that option
  # stays the single source of truth for what is forwarded.
  virtualisation.qemu.networkingOptions =
    let
      forwarding = lib.concatMapStrings
        ({ proto, from, host, guest, ... }:
          if from == "host"
          then
            "hostfwd=${proto}:${host.address}:${toString host.port}-"
            + "${guest.address}:${toString guest.port},"
          else
            throw ("boards/qemu-riscv-virt.nix rebuilds only host-side "
              + "forwards; add guestfwd support here if you need it"))
        config.virtualisation.forwardPorts;
    in
    lib.mkForce [
      "-device virtio-net-pci,netdev=user.0"
      ''-netdev user,id=user.0,${forwarding}"$QEMU_NET_OPTS"''
    ];

  environment.systemPackages = with pkgs; [
    pciutils # virt is a PCIe machine; lspci actually shows something
    strace
    tmux
  ];

  # Same reason modules/sg2002-sd-image.nix sets this: the installer
  # tools drag cross-built helper payloads into the closure, and
  # bcachefs-tools does not cross-compile to riscv64 (rust-bindgen:
  # "unknown target triple 'riscv64gc-unknown-linux-gnu'"). A throwaway
  # VM is not a self-reconfiguring system either way.
  system.disableInstallerTools = true;

  # 256 MiB and a TCG-emulated single core: keep the boot surface small.
  environment.defaultPackages = lib.mkForce [ ];
  documentation.enable = lib.mkDefault false;
  documentation.nixos.enable = lib.mkDefault false;
  services.udisks2.enable = lib.mkDefault false;
  networking.hostName = lib.mkDefault "c906-virt";
  networking.firewall.allowedTCPPorts = [ 22 ];

  system.stateVersion = lib.mkDefault "25.11";
}
