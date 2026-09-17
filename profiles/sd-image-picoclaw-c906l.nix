# Persistent SD image for the PicoClaw LCD owned by the SG2002 C906L.
#
# Linux owns SDIO while the C906L-owned regulator alone switches its GPIOA26
# power rail; see docs/sg2002-c906l-picoclaw-sd-image.md before changing that
# boundary.
{
  config,
  lib,
  rootAuthorizedKeys ? [],
  rootWpaConf ? null,
  ...
}: {
  imports = [
    ./sd-image-mainline.nix
    ../modules/picoclaw-c906l-lcd.nix
    ../modules/sg2002-watchdog-keeper.nix
    ../modules/wifi-aic8800.nix
  ];

  assertions = [
    {
      assertion = config.sg2002.authorizedKeys != [];
      message = ''
        The PicoClaw C906L SD image is key-only. Supply a public-key file with
        NANOKVM_AUTHORIZED_KEYS=/absolute/path/authorized_keys and --impure,
        or provide sg2002.authorizedKeys from a downstream configuration.
      '';
    }
  ];

  sg2002 = {
    wifi = {
      # An explicitly supplied config is installed here at build time.  With
      # none, wpa_supplicant is conditionally skipped until the administrator
      # provisions this root-only path through the authenticated USB link.
      wpaConf = lib.mkDefault rootWpaConf;
      wpaConfRuntimePath = "/etc/wpa_supplicant/wpa_supplicant-wlan0.conf";
    };

    # Unlike a tethered live image, an SD card must continue to run while USB
    # is unplugged.  The keeper therefore supervises the hardware watchdog
    # without treating loss of the development host as a fault.
    watchdogKeeper = {
      initrd.enable = true;
      stage2.enable = true;
      healthHost = null;
    };

    # The SD profile prunes initrd module lists.  PID 1 carries its initrd
    # modules-load state across switch-root, so these exact modules must be
    # both present and loaded before stage 2 rather than relying on
    # boot.kernelModules to be replayed later.
    initrd.availableKernelModules = [
      "sg2002-c906l-control"
      "sg2002-c906l-remoteproc"
      "sg2002-c906l-wifi-power"
      "sg2002-c906l-framebuffer"
    ];
    initrd.kernelModules = [
      "sg2002-c906l-control"
      "sg2002-c906l-remoteproc"
      "sg2002-c906l-wifi-power"
      "sg2002-c906l-framebuffer"
    ];
  };

  networking.hostName = lib.mkOverride 900 "picoclaw-c906l-lcd";

  # This image is intentionally usable only through an explicitly provided
  # public key.  It must never inherit the generic SD profile's development
  # password, particularly because the USB ECM link has no network firewall.
  services.openssh.settings = {
    PermitRootLogin = lib.mkForce "prohibit-password";
    PasswordAuthentication = lib.mkForce false;
    KbdInteractiveAuthentication = lib.mkForce false;
  };
  users.mutableUsers = false;
  users.users.root = {
    initialPassword = lib.mkForce null;
    hashedPassword = lib.mkForce "!";
  };
}
