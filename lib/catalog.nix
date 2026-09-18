# Public SG2002 images are RAM-only; deployment policy belongs to consumers.
{ lib }:
let
  wifi = [
    ../modules/wifi-aic8800.nix
    ../modules/sg2002-initrd-wifi.nix
  ];
  entry = name: boardName: variant: mixins: modules: {
    path = [ name "mainline" "initrd" variant ];
    kernel = "mainline";
    profile = "usb-initrd";
    artifact = "initrd";
    tag = "${name}-${variant}";
    inherit boardName mixins modules;
  };
in [
  (entry "licheerv" "licheerv-nano-w" "default" wifi [ ])
  (entry "pcie" "nanokvm-pcie" "default" wifi [ ])
  (entry "picoclaw" "licheerv-nano-picoclaw" "default"
    (wifi ++ [ ../modules/picoclaw-c906l-lcd.nix ]) [ ])
  (entry "licheerv" "licheerv-nano-w" "c906l-all-timers" [ ] [
    ({ ... }: {
      sg2002.auxCore = {
        enable = true;
        peripherals = [ "timer4" "timer5" "timer6" "timer7" ];
      };
    })
  ])
]
