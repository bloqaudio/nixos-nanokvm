# PicoClaw U-Boot ST7789 splash

`sg2002-fip-mainline-picoclaw-splash` is a separate FIP for the Sipeed
LicheeRV-Nano PicoClaw.  Its U-Boot runs `picoclaw_splash; fastboot usb 0`:
the command performs the PicoClaw Ethernet-pad handoff, initialises the
240x240 ST7789, and draws a small procedural RGB565 graphic before the normal
fastboot FIT staging flow starts.

This is deliberately not part of `sg2002-fip-mainline-fastboot` or the generic
Nano U-Boot image.  Only the `picoclawSplash` U-Boot variant contains the
command, the private DTB, or writes to the LCD's EPHY/GPIO pads.  The catalog
selects `sg2002-usb-boot-picoclaw-splash` only when
`entry.boardName == "licheerv-nano-picoclaw"`; its `usb-boot-mainline` script
pushes the splash FIP after ROM USB-DL.  Generic Nano, camera, and PCIe entries
retain `sg2002-usb-boot` and its normal fastboot FIP.

Limitations:

- The panel sequence and pad values are ported from the repository's known
  Linux userspace PicoClaw driver, but this exact U-Boot image has not been
  tested on physical hardware.
- The splash is procedural rather than a logo asset; it has no text, progress
  reporting, input handling, or error recovery beyond U-Boot command output.
- It relies on the upstream DesignWare SPI and GPIO drivers and directly muxes
  the three control pads because upstream U-Boot lacks the relevant CV18xx
  pinctrl support.
- It leaves the panel enabled and the routed pins idle for the Linux DT/driver
  to take ownership.  It does not attempt to restore Ethernet pad routing;
  that is correct only for PicoClaw, which is why the image is private.
- PicoClaw's catalog board is USB-only (no SD card path is configured), so no
  SD image selection is changed.
