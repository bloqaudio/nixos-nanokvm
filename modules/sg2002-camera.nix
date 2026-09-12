# GC4653 camera kernel/module-load contract. Keep this separate from the
# shared Coda mixin: NanoKVM-PCIe also uses Coda but has no GC4653 overlay.
{
  config,
  lib,
  ...
}: {
  config = lib.mkIf (config.sg2002.kernel == "mainline") {
    # VIDEO_GC4653 is intentionally modular in the lean kernel config.
    # Camera profiles must request it explicitly in both initrd and stage 2;
    # systemd-modules-load does not replay boot.kernelModules after switch-root,
    # and udev cannot infer a module from a media graph before the subdevice
    # exists.
    boot.kernelModules = [ "gc4653" "sg2002-vpss" ];
    sg2002.initrd.availableKernelModules = [ "gc4653" ];
    sg2002.initrd.kernelModules = [ "gc4653" ];
  };
}
