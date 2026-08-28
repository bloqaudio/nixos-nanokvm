{
  config,
  lib,
  ...
}: {
  options.sg2002.usbGadget = {
    product = lib.mkOption {
      type = lib.types.str;
      default = "Sipeed SG2002 (NixOS)";
      description = "USB gadget iProduct string (board-specific; e.g. \"Sipeed NanoKVM-PCIe (NixOS)\").";
    };
    manufacturer = lib.mkOption {
      type = lib.types.str;
      default = "Sipeed";
      description = "USB gadget iManufacturer string.";
    };
    serial = lib.mkOption {
      type = lib.types.str;
      default = "sg2002-0001";
      description = "USB gadget iSerialNumber string.";
    };
    console.enable = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Route the kernel console to the ACM function when CONFIG_U_SERIAL_CONSOLE is available.";
    };
  };

  options.sg2002.usbGadget.network = {
    enable = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Include a network function in the gadget. When disabled, only the ACM serial function is exposed -- useful for bare-console diagnostic boots.";
    };
    transport = lib.mkOption {
      type = lib.types.enum ["ecm" "rndis" "ncm"];
      default = "ecm";
      description = ''
        USB framing protocol for the gadget's network function. All
        three use the same `dev_addr`/`host_addr` configfs surface;
        only the function-driver and frame format differ.

        - "ecm": CDC-ECM (vendor-neutral, vanilla). Linux host binds
          `cdc_ether`. One Ethernet frame per USB bulk transfer.
        - "rndis": Microsoft RNDIS. Linux host binds `rndis_host`.
          Microsoft-style message framing; different f_*-driver code
          path in dwc2 than ECM.
        - "ncm": CDC-NCM (Network Control Model). Linux host binds
          `cdc_ncm`. Aggregates multiple Ethernet frames per USB
          transfer (NDP -- Network Datagram Pointer block). Lowest
          per-frame overhead of the three for high-throughput
          traffic.

        Try all three under NBD load; the answer's empirical.
      '';
    };
    controlFile = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      description = ''
        Optional stage-2 runtime flag file. When set, the stage-2
        gadget includes the network function only while this file
        exists. This is intended for compatibility with user-space UI
        toggles; initrd gadgets remain fully declarative.
      '';
    };
  };

  options.sg2002.usbGadget.initrd.network.enable = lib.mkOption {
    type = lib.types.bool;
    default = config.sg2002.usbGadget.network.enable;
    defaultText = lib.literalExpression "config.sg2002.usbGadget.network.enable";
    description = "Include the network function in the initrd gadget. Disable this for normal SD boots where stage 2 owns USB networking.";
  };

  options.sg2002.usbGadget.stage2.enable = lib.mkOption {
    type = lib.types.bool;
    default = false;
    description = "Bring up the SG2002 debug USB gadget again in stage 2.";
  };

  options.sg2002.usbGadget.stage2.preserveInitrd = lib.mkOption {
    type = lib.types.bool;
    default = false;
    description = ''
      Keep an identical initrd gadget bound across switch-root instead of
      detaching and recreating it. This avoids dropping an active ACM kernel
      console and requires the initrd and stage 2 to expose the same function
      set without a runtime network control file.
    '';
  };

  options.sg2002.usbGadget.stage2.rxGuard.enable = lib.mkOption {
    type = lib.types.bool;
    default = false;
    description = ''
      Detect the SG2002 DWC2 bulk-OUT runtime wedge by probing the USB host,
      then re-probe the controller after two transmitted probes make no
      receive progress. The guard stays idle while USB has no carrier.
    '';
  };

  options.sg2002.usbGadget.stage2.reenumerateAfterBoot = {
    enable = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Restart the stage-2 gadget once after boot. Some SG2002 dwc2
        hosts enumerate the initial stage-2 ECM function but leave the
        link without carrier until the gadget is rebound.
      '';
    };
    delaySec = lib.mkOption {
      type = lib.types.ints.positive;
      default = 300;
      description = "Seconds after boot before the one-shot stage-2 gadget re-enumeration.";
    };
  };
}
