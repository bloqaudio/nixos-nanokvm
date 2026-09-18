# nixpkgs' mainline U-Boot for the Sipeed LicheeRV Nano (SG2002).
# Layered on top of the upstream `sipeed_licheerv_nano_defconfig`:
#   - extraConfig enables USB gadget + fastboot so distro_bootcmd can
#     fall through to "fastboot usb 0" as a recovery channel
#   - 4 local patches (see ./patches/) fix missing ramdisk_addr_r,
#     add an -u-boot.dtsi for the dwc2 gadget, and let the dwc2_udc_otg
#     driver build on RISC-V
{
  buildUBoot,
  lib,
  bootCommand ? "sysboot mmc 0:2 any 0x80c00000 /boot/extlinux/extlinux.conf; run distro_bootcmd; fastboot usb 0",
  # Bytes at the top of DRAM which U-Boot and the subsequently booted OS must
  # leave untouched.  The C906L firmware package uses this for its firmware
  # and shared-memory carveouts; ordinary images retain upstream's zero.
  memoryTopHide ? 0,
  picoclawSplash ? false,
}:
assert lib.assertMsg (builtins.isInt memoryTopHide)
  "sg2002 U-Boot: memoryTopHide must be an integer byte count";
assert lib.assertMsg (memoryTopHide >= 0)
  "sg2002 U-Boot: memoryTopHide must not be negative";
assert lib.assertMsg (lib.mod memoryTopHide 4096 == 0)
  "sg2002 U-Boot: memoryTopHide must be aligned to the 4 KiB OS page size";
buildUBoot {
  defconfig = "sipeed_licheerv_nano_defconfig";
  extraMeta.platforms = ["riscv64-linux"];
  filesToInstall = ["u-boot.bin" "u-boot.dtb"];

  extraConfig = ''
    # extlinux lives on the Btrfs root partition.  The generic filesystem
    # layer used by `sysboot ... any` needs the Btrfs reader compiled in.
    CONFIG_FS_BTRFS=y

    CONFIG_USB=y
    CONFIG_DM_USB=y
    CONFIG_DM_USB_GADGET=y
    CONFIG_USB_GADGET=y
    CONFIG_USB_GADGET_MANUFACTURER="Sipeed"
    CONFIG_USB_GADGET_VENDOR_NUM=0x18d1
    CONFIG_USB_GADGET_PRODUCT_NUM=0xd00d
    CONFIG_USB_GADGET_DWC2_OTG=y
    CONFIG_USB_GADGET_DOWNLOAD=y
    CONFIG_FASTBOOT=y
    CONFIG_FASTBOOT_BUF_ADDR=0x82000000
    CONFIG_FASTBOOT_BUF_SIZE=0x4000000
    CONFIG_FASTBOOT_USB_DEV=0
    CONFIG_USB_FUNCTION_FASTBOOT=y
    CONFIG_CMD_FASTBOOT=y
    # `fastboot oem run "<cmd>"` executes arbitrary U-Boot commands and
    # returns output — our only debug channel once UART is unpopulated.
    CONFIG_FASTBOOT_OEM_RUN=y
    # Console ring buffer readable via `fastboot oem console`; captures
    # the pre-fastboot FSBL/OpenSBI/U-Boot output for post-mortem.
    CONFIG_CONSOLE_RECORD=y
    # MMC tracing and a complete extlinux attempt easily exceed 8 KiB. Keep
    # enough history for `fastboot oem console` to remain useful after a
    # failed kernel, initrd and FDT load sequence.
    CONFIG_CONSOLE_RECORD_OUT_SIZE=0x40000
    CONFIG_CONSOLE_RECORD_IN_SIZE=0x800
    CONFIG_FASTBOOT_CMD_OEM_CONSOLE=y
    # The upstream board defconfig fixes SYS_CBSIZE at 512 bytes. NixOS
    # extlinux APPEND lines routinely exceed that once an init store path and
    # deployment kernel parameters are included; pxe_utils otherwise abandons the
    # label with "bootarg overflow" after loading its kernel and initrd.
    CONFIG_SYS_CBSIZE=2048
    CONFIG_SYS_PBSIZE=2080
    # SG2002/Sipeed SD images need partition 1 marked active for fip.bin,
    # while NixOS extlinux lives on the Btrfs root partition. U-Boot's distro
    # scan can stop at the active firmware partition, so try the known NixOS
    # root partition explicitly before falling back to the generic scan and
    # then fastboot.
    CONFIG_BOOTCOMMAND="${bootCommand}"
    # MMC command-level tracing into the console record; pr_info/pr_debug
    # on the mmc init failure paths only compile in at LOGLEVEL>=7, so
    # without these a failed `mmc dev 0` is completely silent.
    CONFIG_LOGLEVEL=8
    CONFIG_MMC_TRACE=y
  '' + lib.optionalString (memoryTopHide != 0) ''
    # Keep the auxiliary C906L firmware and its Linux mailbox carveout outside
    # U-Boot's relocation and malloc arenas.  The Linux DT independently
    # reserves the same bytes before its allocator comes online.
    CONFIG_SYS_MEM_TOP_HIDE=0x${lib.toHexString memoryTopHide}
  '' + (if picoclawSplash then ''
    # This is a separate PicoClaw-only build: its command changes the four
    # LCD-wired Ethernet pads and GPIOA19/A27/A28 before entering fastboot.
    # Do not enable it in the generic Nano U-Boot image.
    CONFIG_DEFAULT_DEVICE_TREE="sg2002-licheerv-nano-picoclaw"
    CONFIG_DM_GPIO=y
    CONFIG_DWAPB_GPIO=y
    CONFIG_DM_SPI=y
    CONFIG_DESIGNWARE_SPI=y
  '' else "");

  # buildUBoot's default is `cat extras >> .config`; olddefconfig then
  # resolves Kconfig dependencies for the gadget/fastboot tree.
  postConfigure = ''
    make olddefconfig
  '' + lib.optionalString (memoryTopHide != 0) ''
    # Fail the build if this option is renamed, removed, dependency-gated or
    # otherwise discarded by a future U-Boot Kconfig update.  Silently losing
    # this reservation would let U-Boot overwrite the running C906L image.
    if ! grep -Fxq 'CONFIG_SYS_MEM_TOP_HIDE=0x${lib.toHexString memoryTopHide}' .config; then
      echo "SG2002 top-of-RAM reservation was not preserved by olddefconfig" >&2
      grep '^CONFIG_SYS_MEM_TOP_HIDE=' .config >&2 || true
      exit 1
    fi
    if ! grep -Fxq 'CONFIG_LMB_LIMIT_DMA_BELOW_RAM_TOP=y' .config; then
      echo "SG2002 LMB did not preserve the top-of-RAM reservation" >&2
      exit 1
    fi
  '';

  passthru = {
    # Numeric bytes, intentionally not a formatted Kconfig string, so FIP and
    # firmware packages can assert their complete carveout fits this contract.
    inherit memoryTopHide;
  };

  extraPatches = [
    ./patches/0001-configs-licheerv_nano-define-ramdisk_addr_r-move-fdt.patch
    ./patches/0002-riscv-dts-sg2002-licheerv-nano-b-add-U-Boot-dtsi-wit.patch
    ./patches/0003-usb-gadget-dwc2_udc_otg-treat-ENOENT-as-no-clocks.patch
    ./patches/0004-usb-gadget-dwc2_udc_otg-lift-ARM-only-gate-drop-asm-.patch
    # cv1800b SD won't init under our vendor-FSBL FIP because the upstream
    # MMC driver never programs the cv18xx SD PHY at init (only during
    # tuning). Port the kernel's PHY setup so the card answers ACMD41.
    ./patches/0005-mmc-cv1800b_sdhci-program-cv18xx-sd-phy-at-probe.patch
    # SYS_MEM_TOP_HIDE must constrain LMB/EFI allocations as well as U-Boot's
    # relocation and malloc arenas.
    ./patches/0007-lmb-respect-SYS_MEM_TOP_HIDE-for-allocations.patch
  ] ++ (if picoclawSplash then [
    ./patches/0006-cmd-picoclaw-add-board-scoped-pre-linux-splash.patch
  ] else [ ]);
}
