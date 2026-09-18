# Upload-only USB boot. This runner never configures host interfaces, exports
# a store, starts a server, or keeps a process alive after the handoff.
# Only CI opts out of the key guard, with an explicitly empty authorized-keys
# file. Interactive upload artifacts require the caller's public keys.
{ lib, requireAuthorizedKeys ? true }: pkgs: config:
let
  fip = config.system.build.fipFastboot;
  aux = config.sg2002.auxCore;
  contract = pkgs.sg2002-c906l-contract-for aux.peripherals;
  firmware = aux.firmware;
  bootargs = lib.concatStringsSep " " config.boot.kernelParams;
  rawFit = pkgs.sg2002-boot-fit {
    kernel = config.system.build.kernel;
    fdt = config.sg2002.fdt;
    initrd = "${config.system.build.initialRamdisk}/initrd";
    description = "${config.networking.hostName}: standalone NixOS initrd";
  };
  fit = pkgs.runCommand "${config.networking.hostName}-initrd.itb" {
    nativeBuildInputs = [ pkgs.zstd ];
  } ''
    # Include an unpacked-size gate: compressed size alone can pass FIT
    # staging and still exhaust the early rootfs on a 256 MiB device.
    unpacked=$(zstd -dc ${config.system.build.initialRamdisk}/initrd | wc -c)
    test "$unpacked" -lt $((80 * 1024 * 1024)) || {
      echo "Initrd expands to $unpacked bytes; exceeds the 80 MiB budget" >&2
      exit 1
    }
    # FIT staging starts at 0x82000000; initrd relocation starts at
    # 0x85000000. Reject an overlapping payload, before touching a board.
    test "$(stat -c %s ${rawFit})" -lt $((0x85000000 - 0x82000000)) || {
      echo 'FIT exceeds the safe 48 MiB staging window' >&2
      exit 1
    }
    cp ${rawFit} "$out"
  '';
  usb-boot = pkgs.writeShellApplication {
    name = "usb-boot";
    text = ''
      exec ${pkgs.sg2002-usb-boot-for fip}/bin/usb-boot-mainline \
        ${fit} --uboot-watchdog --bootargs ${lib.escapeShellArg bootargs} "$@"
    '';
  };
  bundle = pkgs.runCommand "${config.networking.hostName}-usb-bundle" { } ''
    mkdir -p "$out/fip" "$out/contract"
    cp ${fit} "$out/boot.itb"
    cp ${fip}/fip.bin "$out/fip/fip.bin"
    cp -r ${pkgs.sg2002-cv181x-usb-dl}/lib/cv181x-usb-dl/rom_usb_dl "$out/rom_usb_dl"
    cp ${../pkgs/sg2002/usb-boot/usb_boot_mainline.py} "$out/usb_boot_mainline.py"
    cp ${../scripts/boot-initrd.py} "$out/boot.py"
    cp ${../scripts/rom-download.py} "$out/rom-download.py"
    chmod +x "$out/rom-download.py"
    cp ${contract}/python/*.py "$out/contract/"
    cp ${pkgs.writeText "bootargs" bootargs} "$out/bootargs"
    ${lib.optionalString aux.enable ''
      cp ${firmware}/${firmware.firmwareFile} "$out/c906l.bin"
    ''}
    cp ${../docs/usb-initrd.md} "$out/README.md"
    cp ${../scripts/usb-boot.Dockerfile} "$out/Dockerfile"
    cd "$out"
    find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
  '';
in
assert lib.assertMsg (lib.all (a: a.assertion) config.assertions)
  (lib.concatMapStringsSep "\n" (a: a.message)
    (lib.filter (a: !a.assertion) config.assertions));
assert lib.assertMsg (!requireAuthorizedKeys || config.boot.initrd.network.ssh.authorizedKeys != [ ])
  "Set NANOKVM_AUTHORIZED_KEYS to a public-key file and build with --impure; this image has no password login.";
{
  inherit fit bundle usb-boot;
  kernel = config.system.build.kernel;
  initrd = config.system.build.initialRamdisk;
}
