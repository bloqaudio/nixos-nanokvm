# Mainline SG2002 hardware ISP

The mainline capture driver offers an explicit `NV21` format for RAW Bayer
sources, alongside the existing repacked RAW12 format. Selecting NV21 routes
the sensor through FE0, BE, the RAW demosaic block (CFA), RGB colour conversion
(CSC), and the YUV output engines (DMA46 for Y, DMA47 for VU). Linux owns all
registers, frame grants, interrupts and buffers. No C906L program or vendor
kernel module is needed by this implementation.

This is a first fixed-settings implementation. It disables statistics DMA,
lens shading, HDR, gamma and temporal processing; it does not implement AE,
AWB, sensor tuning or factory image quality. CFA uses reset tuning parameters;
CSC is explicitly programmed with the SDK's neutral full-range BT.601 matrix
(signed Q10 coefficients and offsets 0,512,512). Register readback and image
validation on silicon are still required to establish correct pixel output.

The existing RAW and HDMI formats remain the defaults. `VIDIOC_S_FMT` selects
the ISP only for Bayer inputs with even dimensions. Format changes are refused
while buffers exist. Both output addresses point into one vb2 allocation; a
buffer is completed at POST frame-done, rather than FE frame-done. Streamoff
waits for the pipeline and both write engines, then resets the ISP before
releasing DMA memory. Frame/DMA errors stop and error the queue.

The camera DT enables VPSS as a separate V4L2 mem2mem device, with its own
fabric-clock references. The bridge's `--isp` option requests NV21 capture and
imports that allocation into VPSS, which scales and writes NV12 directly into
the encoder's DMA-BUF. It defaults to quarter resolution (640×360 for GC4653);
`--size half` selects 1280×720. No CPU demosaic/conversion runs in this mode.

The bridge passes the capture colour tuple to VPSS and Coda, including the
extended V4L2 fields. VPSS reports this tuple on both queues because its YUV
matrix is identity. This fixes V4L2 metadata, not H.264 bitstream signalling:
Coda's current SPS crop rewrite omits VUI. Until VUI support is implemented,
image comparisons must explicitly decode as full-range BT.601 with a linear
transfer function; decoder defaults do not establish correct colour rendering.

## Reversible laboratory boot

Build the dedicated profile:

```sh
nix build .#boards.licheerv.mainline.live.usb-cam-isp.usb-boot
```

The runner serves the read-only NBD root and RAM-boots the mainline image. It
does not flash storage. The profile disables the NanoKVM application and does
not start capture automatically. Its independent initrd and stage-2 watchdog
keepers check reachability of `10.55.0.2`; losing the host releases the hardware
watchdog for recovery. Arm a ROM catcher on the host before experiments which
may stall MMIO, and stop any fastboot keepalive before staging an image.

Identify nodes by driver, rather than relying on probe order:

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/videoX --list-formats-ext
```

First request a bounded ISP-only capture, then run the integrated encoder:

```sh
timeout 30 v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=2560,height=1440,pixelformat=NV21 \
  --stream-mmap=2 --stream-count=30 --stream-to=/dev/null

sg2002-h264-bridge /dev/videoX /dev/videoY \
  --scaler-node /dev/videoZ --isp --capture-buffers 2 --mid-buffers 2 \
  --frames 300 --output /tmp/isp-300.h264
```

The acceptance evidence must include actual completed capture frames, a
decodable 300-frame H.264 stream with VCL/IDR content, image geometry and colour
checks, CPU/RSS measurements, clean streamoff and a subsequent successful
stream restart. A successful cross-build alone does not establish these.

Register references are the pinned CV181x SDK's `vi_reg_fields.h`,
`vi_reg_blocks.h`, `isp_reg.h`, and the `vi/chip/mars/vip/vi_*_ip_ctrl.c`
implementations. The mainline patch uses explicit offsets/masks and the
kernel's own DMA/V4L2 APIs, without the factory module ABI.
