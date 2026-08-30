# LicheeRV/GC4653 kernel lab — strix-4

This directory records the isolated hardware bring-up of the LicheeRV Nano
camera board attached to `strix-4`. The USB path is used only for ROM/FIP/FIT
bootstrap and optional recovery; where the board's RJ45 is present, target
NFS/SSH and camera data use its Ethernet address. Do not use the production
`licheerv` identity (`192.168.23.29`) during lab runs.

Large frame captures and transient logs stay off the repository. Record exact
commands, concise verbatim output, kernel/media evidence, and any failure
signatures here after each run.

## Attachment/topology

On `strix-4`, USB bus `7-1` enumerates the board first as Sipeed's ROM
download gadget (`18d1:d00d`, product `USB download gadget`) and after FIT
boot as `1d6b:0104`, product `Sipeed SG2002 (NixOS)`, with CDC ACM
`/dev/ttyACM0` and CDC Ethernet `usb0` (`10.55.0.2` host /
`10.55.0.1` target). USB is bootstrap/control; the attached RJ45 is Link Up
100 Mb/s full duplex and carries runtime NFS/SSH. DHCP assigned changing
non-production Ethernet leases across resets, so no fixed fleet address was
claimed.

The live target reports `Sipeed LicheeRV Nano B-W`. Dynamic media discovery
(`media-ctl -p`) showed `sg2002-capture` `/dev/video0` with an enabled,
immutable sink link from entity `gc4653 1-0029` `/dev/v4l-subdev0`, in
`SRGGB10_1X10/2560x1440@1/30`. The sensor exposed exactly these controls:

    exposure: min=1 max=1492 step=1 default=1488 value=1488
    vertical_blanking: min=60 max=14943 step=1 default=60 value=60
    analogue_gain: min=1024 max=77648 step=1 default=1024 value=1024

The CSI node negotiated packed 10-bit Bayer as `pRCC`, 2560x1440,
`bytesperline=3840`, `sizeimage=5529600`.

## Kernel/media result

The current camera build uses the GC4653 driver as a module, but the
camera-only mixin puts it in both the initrd and stage-2 module lists
(`modules/sg2002-camera.nix:1-18`; the two camera catalog entries select it at
`lib/catalog.nix:488-524`). This matters on the NFS live flow: stage-2
`systemd-modules-load` does not replay the initrd's `boot.kernelModules`, and
without the initrd request a reboot exposed only the generic CSI node until a
manual `modprobe gc4653`.

The driver fix keeps the vendor mode table in sensor standby, applies cached
V4L2 controls while holding the driver's handler mutex, and only then writes
the stream-on register (`0053-media-i2c-add-GalaxyCore-GC4653-sensor-driver.patch:507-528`).
The VBLANK path updates the exposure ceiling as `native_height + vblank - 8`
and the runtime writes preserve the sensor's 16-bit VTS/exposure and gain
register map
(`pkgs/sg2002/linux-mainline/patches/0053-media-i2c-add-GalaxyCore-GC4653-sensor-driver.patch:344-390`).
The patch builds in the selected
mainline kernel; `git diff --check` is clean.

Fresh reboot with the final camera profile proved automatic sensor binding:

    Linux nanokvm-nfs-live 7.2.0-rc5 ... riscv64
    /etc/modules-load.d/nixos.conf: ... gc4653 ...
    /dev/media0 /dev/v4l-subdev0 /dev/video0
    sg2002-capture ... bound source gc4653 1-0029 pad 0
    CSI errors: 0

Three additional post-reboot STREAMON/STREAMOFF frames completed at the
fixed 2560x1440 pRCC format (`5529600` bytes each); the default-control raw
hash was
`7a52f54579c84d5745fa7f0141b0ff51678cd487a24237f8d4aaa25397d542e1`.
The six control-boundary cases below are the stronger control test (the
minimal final runtime image does not ship `v4l2-ctl`/`media-ctl`).

## Audio side-channel check

The camera overlay itself contains only the GC4653 I2C/clock/reset and MIPI
CSI endpoint (`pkgs/sg2002/dtb-mainline/sg2002-licheerv-camera-gc4653.dtsi:193-215`);
there is no microphone or audio endpoint on the camera module. The carrier
DT describes the LicheeRV Nano's internal RXADC/I2S0 and TXDAC/I2S3, their
DMA channels, and the `sg2002-onboard` simple-audio-card
(`pkgs/sg2002/dtb-mainline/sg2002-licheerv-nano-bw.dtsi:344-439`). This is
carrier-board audio, not GC4653 audio. The live DT model identified the
attached carrier as `Sipeed LicheeRV Nano B-W`.

The normal low-memory camera profile intentionally leaves `CONFIG_SOUND` off
(`pkgs/sg2002/linux-mainline/config.nix:268-280`). For this isolated lab only,
I built a kernel from the same source and patches with these eight symbols
built in: `SOUND`, `SND`, `SND_PCM`, `SND_SOC`, `SND_SIMPLE_CARD`,
`SND_SOC_CV1800B_TDM`, `SND_SOC_CV1800B_ADC_CODEC`, and
`SND_SOC_CV1800B_DAC_CODEC`. The generated kernel config was accepted and the
Image grew by 3,584 bytes (19,802,624 vs 19,799,040); no fleet profile was
changed.

Verbatim live-kernel enumeration:

    CONFIG_SOUND=y
    CONFIG_SND=y
    CONFIG_SND_PCM=y
    CONFIG_SND_DMAENGINE_PCM=y
    CONFIG_SND_SOC=y
    CONFIG_SND_SOC_GENERIC_DMAENGINE_PCM=y
    CONFIG_SND_SOC_CV1800B_TDM=y
    CONFIG_SND_SOC_CV1800B_ADC_CODEC=y
    CONFIG_SND_SOC_CV1800B_DAC_CODEC=y
    0 [sg2002onboard  ]: simple-card - sg2002-onboard
                          sg2002-onboard
    00-00: cv1800b-i2s-dac-hifi dac-hifi-0 : cv1800b-i2s-dac-hifi dac-hifi-0 : playback 1
    00-01: cv1800b-i2s-adc-hifi adc-hifi-1 : cv1800b-i2s-adc-hifi adc-hifi-1 : capture 1

The kernel-side ALSA test is the small RISC-V binary built from
`alsa-kernel-test.c`. As root on the live target it completed both directions
at 48 kHz, stereo S16_LE, with no recovery/error path:

    camera_capture capture channels=2 rate=48000 period=1024
    capture_frames=144000 bytes=576000 sample_min=-140 sample_max=128 nonzero_samples=283625 rms=27.04
    board_playback playback channels=2 rate=48000 period=1024
    playback_frames=48000 bytes=192000
    alsa_test_rc=0
    576000 /run/camera-audio.raw
    7a4068e08f8fb273b50d0459b43dfae51cacebcb91dab442f6fc2a611582d680  /run/camera-audio.raw

The capture had non-zero samples (not a zero-filled fake stream), and the
playback PCM accepted and drained 48,000 frames of silence through the DAC
path. The raw file was deleted after hashing. This proves the carrier's
kernel/ALSA RX and TX DMA paths; it does not claim an audible speaker test.
