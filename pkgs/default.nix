# Overlay for everything this flake adds to nixpkgs.
#
# Three halves:
#   1. `spacemit-k3-*` — board-support for SpacemiT K3 systems.
#   2. `sg2002-*` — board-support for the Sophgo CV181x family
#      (kernel builds, FIP, OpenSBI, U-Boot, AIC8800 driver/firmware,
#      USB-recovery tool, DTBs). Inlined here from nixos-sg2002 so
#      this repo is self-contained.
#   3. `nanokvm-*` — the userspace bits (the Go server, the web
#      bundle, the erofs rootfs builder, the kexec payload format)
#      that turn a CV181x board into a working KVM.
{ inputs
, nanokvmPatches ? [ ]
,
}: final: prev:
let
  inherit (final) lib;

  # Cross-compile context. When the overlay is applied to riscv64
  # pkgs (NixOS hostPlatform=riscv64) `final` already is the cross
  # set; on x86_64 build-host evaluation `pkgsCross.riscv64` is the
  # equivalent.
  cross =
    if final.stdenv.hostPlatform.isRiscV64
    then final
    else final.pkgsCross.riscv64;

  mainlineLinuxSource = final.buildPackages.callPackage ./sg2002/linux-mainline/source.nix { };
  dtbMainline = final.buildPackages.callPackage ./sg2002/dtb-mainline {
    linuxSrc = mainlineLinuxSource.src;
  };
  dtbVendor = final.buildPackages.callPackage ./sg2002/dtb-vendor {
    licheerv-nano-build = inputs.licheerv-nano-build;
  };

in
{
  # -----------------------------------------------------------------
  # nixpkgs adjustments
  # -----------------------------------------------------------------

  kexec-tools =
    if final.stdenv.hostPlatform.system == "riscv64-linux"
    then
      prev.kexec-tools.overrideAttrs
        (old: {
          meta =
            (old.meta or { })
            // {
              # kexec-tools 2.0.32 builds for riscv64; nixpkgs still
              # carries a stale badPlatforms entry for this target.
              badPlatforms = lib.remove "riscv64-linux" (old.meta.badPlatforms or [ ]);
            };
        })
    else prev.kexec-tools;

  # vmtouch's Makefile invokes `pod2man` to render the manpage, but the
  # cross stdenv doesn't carry perl. Skip the manpage and only install
  # the binary — that's all `prepare-kexec-stage.service` needs.
  vmtouch =
    if final.stdenv.hostPlatform.system == "riscv64-linux"
    then
      prev.vmtouch.overrideAttrs
        (old: {
          buildPhase = ''
            runHook preBuild
            $CC -Wall -O2 -g -std=c99 -o vmtouch vmtouch.c
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 vmtouch "$out/bin/vmtouch"
            runHook postInstall
          '';
        })
    else prev.vmtouch;

  # -----------------------------------------------------------------
  # spacemit-k3-* (board support)
  # -----------------------------------------------------------------

  "spacemit-k3-linux" = cross.callPackage ./spacemit-k3/linux {
    kernelPatches = [ ];
  };
  "linuxPackages_spacemit-k3" = cross.linuxPackagesFor final."spacemit-k3-linux";
  "spacemit-k3-fsbl" = cross.callPackage ./spacemit-k3/fsbl { };
  "spacemit-k3-rtw89-firmware" = final.callPackage ./spacemit-k3/rtw89-firmware { };
  "spacemit-k3-uefi-blobs" = final.callPackage ./spacemit-k3/uefi-blobs { };
  "spacemit-k3-raw-fastboot-boot" = final.callPackage ./spacemit-k3/raw-fastboot-boot { };
  "spacemit-k3-flash-uefi" = final.callPackage ./spacemit-k3/flash-uefi {
    uefiBlobs = final."spacemit-k3-uefi-blobs";
  };

  # -----------------------------------------------------------------
  # nanokvm-* (userspace KVM stack)
  # -----------------------------------------------------------------

  nanokvm-patched-src = final.callPackage ./nanokvm-patched-src {
    src = inputs.nanokvm-src;
    patches = nanokvmPatches;
  };

  # Static web assets are built on the build host and copied into the
  # target server package; do not cross-build Node/V8 for riscv64.
  nanokvm-web = final.buildPackages.callPackage ./nanokvm-web {
    nanokvm-patched-src = final.nanokvm-patched-src;
  };
  nanokvm-factory-runtime = final.callPackage ./nanokvm-factory-runtime { };
  nanokvm-host-keys = final.callPackage ./nanokvm-host-keys { };
  sg2002-coda980-firmware = final.callPackage ./sg2002/coda980-firmware { };
  nanokvm-server = final.callPackage ./nanokvm-server { };

  # Build with -tags nocamera so libkvm.so isn't linked in at all —
  # its C++ static constructors SEGV in SAMPLE_COMM_VI_ParseIni when
  # run against a mainline kernel.
  nanokvm-server-nocamera = final.nanokvm-server.override { noCamera = true; };

  # Device server, forced to riscv64. Must be instantiated via
  # buildPackages.callPackage (build host) with an explicit targetSystem,
  # NOT `.override` on the cross-spliced pkgs.nanokvm-server — the splice
  # silently drops override args, so that path builds an x86_64 binary
  # that dies 203/EXEC on the device. nocamera = pure-Go cross-compile.
  nanokvm-server-device = final.callPackage ./nanokvm-server {
    noCamera = true;
    targetSystem = "riscv64-linux";
  };

  # Device server with the camera/HDMI capture path compiled in —
  # the build the vendor-kernel images run. Same forced-riscv64
  # instantiation rationale as nanokvm-server-device above; the only
  # difference is noCamera = false, which keeps the kvm_vision cgo
  # binding (libkvm.so) linked in and ships the vendor dl_lib blobs,
  # the prebuilt kvm_system binary, and the LT6911 sensor INI. Only
  # safe with sg2002.kernel = "vendor": libkvm.so's C++ static ctors
  # SEGV under mainline (see nanokvm-server-nocamera above).
  nanokvm-server-device-camera = final.callPackage ./nanokvm-server {
    noCamera = false;
    targetSystem = "riscv64-linux";
  };

  nbd-client-minimal = final.callPackage ./nbd-client-minimal { };

  nanokvm-erofs-rootfs-for = toplevel:
    final.callPackage ./erofs-rootfs {
      inherit toplevel;
    };

  # Throughput benchmark for the USB transport. Wrapped as a
  # writeShellApplication so its closure carries jq+iperf3+fio+ssh
  # by reference; `nix run .#nanokvm-bench-usb-transport` works
  # without nested `nix shell` calls that add eval noise to the
  # measurement.
  nanokvm-bench-usb-transport = final.writeShellApplication {
    name = "nanokvm-bench-usb-transport";
    runtimeInputs = with final; [
      coreutils
      fio
      gnugrep
      gnused
      iperf3
      iputils # ping
      jq
      openssh
    ];
    text = builtins.readFile ../scripts/bench-usb-transport.sh;
  };

  nanokvm-kexec-payload-erofs = args:
    final.callPackage ./kexec-payload-erofs args;

  sophgo-host-tools = final.callPackage ./sophgo-host-tools { };

  # -----------------------------------------------------------------
  # sg2002-* (board support, inlined from nixos-sg2002)
  # -----------------------------------------------------------------

  # Mainline OpenSBI with our U-Boot DTB baked in so OpenSBI has an
  # FDT even when the FSBL doesn't pass one via fw_dynamic_info.
  sg2002-opensbi-mainline = cross.opensbi.override {
    withFDT = "${cross.sg2002-uboot-mainline}/u-boot.dtb";
  };

  # Sophgo's fiptool: source-only package. Loaded via the pinned
  # flake input — fiptool is Python + bundled FSBL/DDR blobs.
  sg2002-sophgo-fiptool = inputs.sophgo-fiptool;

  # Vendor FIP (FSBL + vendor OpenSBI + vendor U-Boot) extracted from
  # a known-good Sipeed SD image. ROM loads fip.bin from FAT partition;
  # we lift it back out with mcopy so we have a working FSBL+DDR blob
  # baseline for the mainline-uboot rebuild below.
  sg2002-fip =
    let
      sipeedImage = final.fetchurl {
        url = "https://github.com/sipeed/LicheeRV-Nano-Build/releases/download/20251202/2025-12-02-16-54-27b96a.img.xz";
        hash = "sha256-D9jDObp9/luqVZ/907bt8WkQVMhj4+LsaV/cZA0y/No=";
      };
    in
    final.runCommand "fip-sg2002"
      {
        # buildPackages: these run on the build host, not the riscv64
        # target.
        nativeBuildInputs = with final.buildPackages; [ xz mtools ];
      } ''
      mkdir -p $out
      xz -dc ${sipeedImage} | dd bs=1M count=20 iflag=fullblock of=image.bin status=none
      dd if=image.bin of=fat.img bs=512 skip=1 count=32768 status=none
      mcopy -i fat.img -n ::fip.bin $out/fip.bin
    '';

  # Vendor FSBL/DDR + mainline OpenSBI + mainline U-Boot, repacked
  # via sophgo's fiptool. This is what the USB recovery flow loads.
  sg2002-fip-mainline-uboot = final.callPackage ./sg2002/fip-mainline-uboot {
    sg2002-fip = final.sg2002-fip;
    sg2002-sophgo-fiptool = final.sg2002-sophgo-fiptool;
    sg2002-opensbi-mainline = cross.sg2002-opensbi-mainline;
    sg2002-uboot-mainline = cross.sg2002-uboot-mainline;
  };
  sg2002-fip-mainline-fastboot = final.callPackage ./sg2002/fip-mainline-uboot {
    sg2002-fip = final.sg2002-fip;
    sg2002-sophgo-fiptool = final.sg2002-sophgo-fiptool;
    sg2002-opensbi-mainline = cross.sg2002-opensbi-mainline;
    sg2002-uboot-mainline = cross.sg2002-uboot-mainline-fastboot;
  };
  # PicoClaw's ST7789 needs the Ethernet-pad handoff before fastboot starts.
  # Keep this complete U-Boot/OpenSBI/FIP chain separate from every generic
  # SG2002 image so those images cannot write the panel's pins.
  sg2002-opensbi-mainline-picoclaw-splash = cross.opensbi.override {
    withFDT = "${cross.sg2002-uboot-mainline-picoclaw-splash}/u-boot.dtb";
  };
  sg2002-fip-mainline-picoclaw-splash = final.callPackage ./sg2002/fip-mainline-uboot {
    sg2002-fip = final.sg2002-fip;
    sg2002-sophgo-fiptool = final.sg2002-sophgo-fiptool;
    sg2002-opensbi-mainline = cross.sg2002-opensbi-mainline-picoclaw-splash;
    sg2002-uboot-mainline = cross.sg2002-uboot-mainline-picoclaw-splash;
  };

  # AIC8800DC firmware blobs (Nano-W onboard WiFi+BT). passthru
  # `compressFirmware=false` because aicbsp's rwnx_load_firmware uses
  # filp_open on the literal .bin filename — the .zst suffix nixpkgs
  # would add breaks the driver's open().
  sg2002-aic8800-firmware =
    final.runCommand "aic8800-firmware"
      {
        passthru.compressFirmware = false;
      } ''
      mkdir -p $out/lib/firmware/aic8800_sdio
      cp -rL ${inputs.aic8800-firmware-src}/* $out/lib/firmware/aic8800_sdio/
      chmod -R u+w $out/lib/firmware/aic8800_sdio

      cd $out/lib/firmware/aic8800_sdio/aic8800DC
      # The Nano-W radio identifies as AIC8800D80 (SDIO 0xc8a1:0x0082),
      # while the Radxa driver is deliberately built with this compatibility
      # directory as CONFIG_AIC_FW_PATH. Expose the complete D80 set here;
      # keep any DC-specific file that already exists under the same name.
      for blob in ../aic8800_and_aic8800D80/*; do
        name="''${blob##*/}"
        test -e "$name" || ln -s "$blob" "$name"
      done
      ln -sfn ../aic8800_and_aic8800D80/fw_adid_u03.bin         fw_adid_u03.bin
      ln -sfn ../aic8800_and_aic8800D80/fw_patch_u03.bin        fw_patch_u03.bin
      ln -sfn ../aic8800_and_aic8800D80/fw_patch_table_u03.bin  fw_patch_table_u03.bin
      ln -sfn ../aic8800_and_aic8800D80/fmacfw.bin              fmacfw.bin
      ln -sfn ../aic8800_and_aic8800D80/fmacfw_patch.bin        fmacfw_patch.bin
      ln -sfn aic_userconfig_8800dc.txt                         aic_userconfig.txt
    '';

  # Sophgo CV181x USB download tool (cv181x-dl + cv181x-rom-dl). Used
  # by the USB-recovery boot flow to FSBL-stream FIP+kernel+initrd.
  sg2002-cv181x-usb-dl =
    let
      pythonEnv = final.python3.withPackages (ps: [ ps.pyserial ps.pyusb ]);
    in
    final.stdenv.mkDerivation {
      pname = "cv181x-usb-dl";
      version = "0.1.0";
      src = "${inputs.licheerv-nano-build}/build/tools/cv181x/usb_dl";
      nativeBuildInputs = [ final.buildPackages.makeWrapper final.buildPackages.python3 ];

      # Upstream cv181x_rom_usb_download.py drops into an infinite
      # "Connecting to ROM 2nd stage..." loop after pushing the first
      # FIP chunk + TX_FLAG + BREAK, polling for vendor `cvi_utask`
      # which never appears with mainline U-Boot. The wrapper that
      # invokes this tool (`usb_boot_mainline.py`) wants the rom-dl
      # process to *exit* once the FIP push is done so it can move on
      # to fastboot enumeration. Patch: short-circuit the 2nd-stage
      # loop to `sys.exit(0)` immediately after BREAK. Bytes-for-bytes
      # of FIP go through unchanged; we just skip the dead polling.
      postPatch = ''
        # Applies the pyserial fast-open / short-timeout / flushOutput-EIO
        # fixes (and, if ever re-enabled, the 2nd-stage skip).
        python3 ${./sg2002-cv181x-rom-dl-skip-2nd-stage.py} \
          rom_usb_dl/cv181x_rom_usb_download.py
      '';

      installPhase = ''
        mkdir -p $out/lib/cv181x-usb-dl $out/bin
        cp -r rom_usb_dl     $out/lib/cv181x-usb-dl/
        cp cv181x_dl.py      $out/lib/cv181x-usb-dl/
        makeWrapper ${pythonEnv}/bin/python3 $out/bin/cv181x-dl \
          --add-flags "$out/lib/cv181x-usb-dl/cv181x_dl.py" \
          --prefix PYTHONPATH : "$out/lib/cv181x-usb-dl:$out/lib/cv181x-usb-dl/rom_usb_dl"
        makeWrapper ${pythonEnv}/bin/python3 $out/bin/cv181x-rom-dl \
          --add-flags "$out/lib/cv181x-usb-dl/rom_usb_dl/cv181x_rom_usb_download.py" \
          --prefix PYTHONPATH : "$out/lib/cv181x-usb-dl/rom_usb_dl"
        makeWrapper ${pythonEnv}/bin/python3 $out/bin/cv181x-uboot-dl \
          --add-flags "$out/lib/cv181x-usb-dl/rom_usb_dl/cv181x_uboot_usb_download.py" \
          --prefix PYTHONPATH : "$out/lib/cv181x-usb-dl/rom_usb_dl"
      '';
    };

  sg2002-uboot-mainline = cross.callPackage ./sg2002/uboot-mainline { };
  sg2002-uboot-mainline-fastboot = cross.sg2002-uboot-mainline.override {
    bootCommand = "fastboot usb 0";
  };
  sg2002-uboot-mainline-picoclaw-splash = cross.sg2002-uboot-mainline.override {
    picoclawSplash = true;
    bootCommand = "picoclaw_splash; fastboot usb 0";
  };

  # Normal nixpkgs kernel + SG2002 patches + structured deltas (see
  # ./sg2002/linux-mainline/default.nix). No hand-rendered configfile.
  sg2002-kernel-mainline = cross.callPackage ./sg2002/linux-mainline { };
  # Keep the normal mainline kernel's Bluetooth stack disabled.  The AIC
  # HCI transport is experimental on this board, so only its explicit
  # consumer pays for bluetooth.ko and its protocol dependencies.
  sg2002-kernel-mainline-bluetooth = cross.callPackage ./sg2002/linux-mainline {
    bluetooth = true;
  };
  # The common carrier's onboard RXADC/TXDAC ALSA simple-card path is also
  # opt-in.  Keep separate variants so a Bluetooth-only consumer does not pay
  # for sound, while an audio+Bluetooth consumer gets one ABI-consistent tree.
  sg2002-kernel-mainline-audio = cross.callPackage ./sg2002/linux-mainline {
    audio = true;
  };
  sg2002-kernel-mainline-audio-bluetooth = cross.callPackage ./sg2002/linux-mainline {
    audio = true;
    bluetooth = true;
  };

  # Vendor 5.10 tree with NanoKVM extras (NBD, erofs). Built from
  # licheerv-nano-build's vendor kernel tarball; baseExtraConfig is
  # the upstream sg2002 vendor defconfig snippet, our kernelExtraConfig
  # appends NBD + erofs support and forces the same modDirVersion the
  # vendor userspace expects.
  sg2002-kernel-vendor = prev.callPackage ./sg2002-kernel-vendor-nanokvm {
    baseExtraConfig = ./sg2002/linux-vendor/extra-config.txt;
    licheerv-nano-build = inputs.licheerv-nano-build;
    kernelPatches = [ ];
    modDirVersion = "5.10.4-tag-";
    kernelExtraConfig = ''
      CONFIG_LOCALVERSION="-tag-"
      # CONFIG_LOCALVERSION_AUTO is not set
      CONFIG_BLK_DEV_NBD=y
      CONFIG_EROFS_FS=y
      CONFIG_EROFS_FS_XATTR=y
      CONFIG_EROFS_FS_POSIX_ACL=y
      CONFIG_EROFS_FS_SECURITY=y
      # CONFIG_EROFS_FS_ZIP is not set
    '';
  };

  # DTBs. dtbMainline / dtbVendor return attrsets; destructure into
  # one flat top-level attr per concrete output the overlay exposes.
  sg2002-dtb-mainline = dtbMainline.dtb;
  sg2002-dtbs-mainline = dtbMainline.dtbs;
  sg2002-dtb-mainline-high-speed = dtbMainline.high-speed;
  sg2002-dtb-mainline-eth = dtbMainline.eth;
  sg2002-dtb-mainline-nowifi = dtbMainline.nowifi;
  sg2002-dtb-mainline-nowifi-high-speed = dtbMainline.nowifi-high-speed;
  sg2002-dtb-mainline-oled = dtbMainline.oled;
  sg2002-dtb-mainline-picoclaw-lcd = dtbMainline.picoclaw-lcd;
  sg2002-dtb-mainline-picoclaw-lcd-wifi = dtbMainline.picoclaw-lcd-wifi;
  sg2002-dtb-mainline-picoclaw-lcd-high-speed = dtbMainline.picoclaw-lcd-high-speed;
  sg2002-dtb-mainline-pcie = dtbMainline.pcie;
  sg2002-dtb-mainline-pcie-nowifi = dtbMainline.pcie-nowifi;
  sg2002-dtb-mainline-pcie-high-speed = dtbMainline.pcie-high-speed;
  sg2002-dtb-mainline-cam = dtbMainline.cam;
  sg2002-dtb-vendor = dtbVendor.boot;
  sg2002-dtb-vendor-gadget = dtbVendor.gadget;

  sg2002-boot-fit = final.callPackage ./sg2002/boot-fit { };

  picoclaw-lcd-test = final.callPackage ./sg2002/picoclaw-lcd-test { };
  sg2002-h264-bridge = final.callPackage ./sg2002/h264-bridge { };
  # Separate test derivation: the shared source enables ALSA/PCMA only here;
  # the normal bridge has neither an ALSA header nor a library dependency.
  sg2002-h264-bridge-pcma = final.callPackage ./sg2002/h264-bridge {
    enablePcma = true;
  };
  sg2002-alsa-kernel-test = final.callPackage ./sg2002/alsa-kernel-test { };

  sg2002-usb-boot = final.callPackage ./sg2002/usb-boot {
    sg2002-cv181x-usb-dl = final.sg2002-cv181x-usb-dl;
    sg2002-fip = final.sg2002-fip;
    sg2002-fip-mainline-uboot = final.sg2002-fip-mainline-fastboot;
  };
  # This runner differs only in the FIP sent after ROM USB-DL.  It makes
  # PicoClaw's board-private U-Boot splash reachable without changing any
  # other SG2002 USB boot path.
  sg2002-usb-boot-picoclaw-splash = final.callPackage ./sg2002/usb-boot {
    sg2002-cv181x-usb-dl = final.sg2002-cv181x-usb-dl;
    sg2002-fip = final.sg2002-fip;
    sg2002-fip-mainline-uboot = final.sg2002-fip-mainline-picoclaw-splash;
  };

  # AIC8800 kernel module — vendor and mainline variants, parameterised
  # by the target kernel so callers can match the driver to whichever
  # kernel they're building.
  sg2002-aic8800-vendor-for = kernel:
    cross.callPackage ./sg2002/aic8800-vendor {
      inherit kernel;
      licheerv-nano-build = inputs.licheerv-nano-build;
    };
  sg2002-aic8800-mainline-for = kernel:
    cross.callPackage ./sg2002/aic8800-mainline {
      inherit kernel;
      src = inputs.aic8800-radxa;
      firmware = final.sg2002-aic8800-firmware;
    };
  # Separate derivation so WiFi-only consumers retain their byte-for-byte
  # existing module configuration.  This one turns on the vendor's shared
  # SDIO HCI transport in both BSP and FDRV.
  sg2002-aic8800-mainline-bluetooth-for = kernel:
    cross.callPackage ./sg2002/aic8800-mainline/bluetooth.nix {
      inherit kernel;
      src = inputs.aic8800-radxa;
      firmware = final.sg2002-aic8800-firmware;
    };
}
