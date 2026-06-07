# Mainline-kernel SD-card image.
#
# Unlike profiles/sd-image.nix (vendor 5.10 + vendor-FIT), this boots
# the mainline kernel via mainline U-Boot + extlinux: U-Boot's
# distro_bootcmd scans the ext4 root partition for
# /boot/extlinux/extlinux.conf and loads kernel + dtb + initrd from
# there. fip.bin (mainline U-Boot) lives on the FAT firmware partition.
#
# Reachability caveat: on the mainline kernel this board has no working
# wired ethernet (bm-dwmac is vendor-only), so we bring up the USB-ECM
# gadget in the initrd — that gives `usb0` @ 10.55.0.1 for tethered
# access AND a `ttyGS0` serial console over the same USB-C cable, which
# is our only window into the boot without a UART adapter. WiFi can be
# layered on via the wifi mixin once ./wifi.conf exists.
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

  boot.loader.grub.enable = false;
  boot.loader.generic-extlinux-compatible.enable = true;
  hardware.deviceTree.enable = true;
  # Override the kernel build's default dtbs dir with our SG2002 DTB.
  hardware.deviceTree.package = lib.mkForce pkgs.sg2002-dtbs-mainline;

  # Both sg2002-sd-image.nix and sg2002-usb-gadget-initrd.nix mkForce
  # boot.initrd.{available,}KernelModules; resolve the tie in favour of
  # the gadget's needs with a stronger-than-mkForce override.
  boot.initrd.availableKernelModules = lib.mkOverride 30 [
    "libcomposite"
    "usb_f_ecm"
    "usb_f_acm"
    "configfs"
  ];
  boot.initrd.kernelModules = lib.mkOverride 30 ["libcomposite"];

  # Mirror the kernel console onto the USB gadget serial so the router
  # sees boot output on its ttyACM. (sg2002-sd-image.nix already adds
  # console=ttyS0; kernelParams is a merged list.)
  boot.kernelParams = ["console=ttyGS0,115200"];

  # Interactive login over the USB serial console.
  systemd.services."serial-getty@ttyGS0".enable = true;

  sg2002.authorizedKeys = rootAuthorizedKeys;
  networking.hostName = lib.mkDefault "nanokvm";

  services.openssh = {
    enable = true;
    settings.PermitRootLogin = "yes";
    settings.PasswordAuthentication = true;
  };

  users.users.root.initialPassword = "nixos";

  services.nanokvm = {
    enable = true;
    openFirewall = true;
  };
}
