# Mainline-kernel SD-card image.
#
# Unlike profiles/sd-image.nix (vendor 5.10 + vendor-FIT), this boots
# the mainline kernel via mainline U-Boot + extlinux: U-Boot's
# distro_bootcmd scans the Btrfs root partition for
# /boot/extlinux/extlinux.conf and loads
# kernel + dtb + initrd from there. fip.bin (mainline U-Boot) lives on
# the FAT firmware partition.
#
# Reachability: the NanoKVM-PCIe board module brings up wired Ethernet in the
# initrd and stage 2. One ECM + ACM gadget remains bound across switch-root;
# stage-2 networkd adopts usb0 without a fragile USB disconnect/re-enumeration.
{
  config,
  lib,
  pkgs,
  rootAuthorizedKeys ? [],
  ...
}: {
  imports = [
    ../modules/sg2002-sd-image.nix
    ../modules/sg2002-usb-gadget-initrd.nix
  ];

  # Mainline U-Boot + extlinux, NOT the vendor FIT. (platform default
  # is already "mainline"; be explicit so this profile is self-evident.)
  sg2002.uboot = lib.mkForce "mainline";
  sg2002.usbGadget.network.enable = true;
  # Keep one ECM+ACM gadget bound from initrd through stage 2. Detaching an
  # ACM function used as the kernel console can wait indefinitely for a host
  # reader, while resetting DWC2 under ttyGS0 can wedge stage-2 sysinit.
  sg2002.usbGadget.initrd.network.enable = true;
  sg2002.usbGadget.stage2.enable = true;
  sg2002.usbGadget.stage2.preserveInitrd = true;
  sg2002.usbGadget.stage2.rxGuard.enable = true;

  boot.loader.grub.enable = false;
  boot.loader.generic-extlinux-compatible.enable = true;
  hardware.deviceTree.enable = true;
  # Same single source of truth as the FIT path: wrap config.sg2002.fdt
  # (set by the platform default + WiFi/OLED/ethernet modules) into the
  # dtbs dir extlinux expects, so the SD image and the USB boot-fit
  # always agree on the DTB.
  hardware.deviceTree.name = "sg2002.dtb";
  hardware.deviceTree.package = lib.mkForce (
    pkgs.runCommand "sg2002-fdt-dir" {} ''
      mkdir -p "$out"
      cp ${config.sg2002.fdt} "$out/sg2002.dtb"
    ''
  );

  # Mirror the kernel console onto the USB gadget serial and keep the
  # OpenSBI firmware region reserved, matching the USB FIT boot path.
  # (sg2002-sd-image.nix already adds console=ttyS0; kernelParams is
  # a merged list.)
  # mkAfter keeps ttyGS0 last even when a board adds a physical rescue UART;
  # the final console= entry is the device backing /dev/console.
  boot.kernelParams = lib.mkAfter [
    # ttyGS0 and the board's physical rescue UART both have explicit getty
    # units below/in the board module. Do not create another serial getty for
    # the early UART0 kernel console while its device is still coldplugging.
    "systemd.getty_auto=no"
    "console=ttyGS0,115200"
    "riscv.fwsz=0x80000"
  ];

  # Interactive login over the USB serial console.
  systemd.services."serial-getty@ttyGS0".enable = true;

  sg2002.authorizedKeys = rootAuthorizedKeys;
  networking.hostName = lib.mkDefault "nanokvm";

  services.openssh = {
    enable = true;
    # RSA host-key generation consumed more than a minute on the single-core
    # SG2002. Generate one modern, unique key on the device and persist it.
    hostKeys = [
      {
        path = "/etc/ssh/ssh_host_ed25519_key";
        type = "ed25519";
      }
    ];
    settings.PermitRootLogin = "yes";
    settings.PasswordAuthentication = true;
  };

  users.users.root.initialPassword = "nixos";

  # Keep the native recovery/debug image self-contained.  These are the
  # small interactive tools needed to inspect system pressure and exercise
  # the mainline media graph without borrowing executables over NFS.
  environment.systemPackages = with pkgs; [
    btop
    (v4l-utils.override {
      withGUI = false;
      withBPF = false;
    })
  ];

  services.nanokvm = {
    enable = false;
    openFirewall = false;
  };
}
