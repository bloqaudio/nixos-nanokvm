# PicoClaw C906L LCD SD image

`boards.picoclaw.mainline.sd.c906l-lcd` builds a persistent, mainline-kernel
SD image for the LicheeRV-Nano PicoClaw.  Linux renders through the standard
DRM/KMS and fbdev interfaces; the C906L firmware owns LCD SPI1 and GPIOA,
including the AIC8800's GPIOA26 Wi-Fi power line.  Linux receives an
acknowledged regulator interface for that one power rail rather than direct
access to GPIOA.

The image uses mainline U-Boot/extlinux, a FAT firmware partition beginning at
LBA 1, and a Btrfs root partition.  It is a 4 GiB raw image; use an 8 GiB or
larger card.  On first boot the root partition grows to fill the card.

This composition has build and contract validation.  Simultaneous LCD and
Wi-Fi behaviour still requires an on-hardware SD boot validation; do not treat
a successful build as that proof.

## Build with SSH access

The image is key-only: it refuses to build without at least one public root
key.  Keep the key file outside Git and point the build at its absolute path.
For example, use an existing Ed25519 public key or create a dedicated one:

```sh
ssh-keygen -t ed25519 -f "$HOME/.ssh/picoclaw-sd" -C picoclaw-sd
NANOKVM_AUTHORIZED_KEYS="$HOME/.ssh/picoclaw-sd.pub" \
  nix build --impure .#boards.picoclaw.mainline.sd.c906l-lcd
```

Multiple public-key lines are accepted.  `authorized_keys` at the repository
root remains a convenient ignored local fallback, but an explicit environment
path is more reliable for a Git flake.  The resulting image is under
`result/sd-image/`.

Root permits public-key authentication only.  No fixed password is present.
The board generates a unique Ed25519 SSH host key on its writable SD card at
first boot; do not copy a host key from a USB live image.

## Wi-Fi credentials

The AIC8800 is enabled in this image.  Its power is requested by the SDIO
controller through the C906L-backed regulator, so Linux never writes GPIOA.
There are two intentional provisioning choices.

For a preconfigured card, create an ignored `wpa_supplicant.conf`-format file
with mode 0600, then build impurely:

```conf
ctrl_interface=DIR=/run/wpa_supplicant GROUP=wheel
network={
  ssid="example-ssid"
  psk="example-passphrase"
}
```

```sh
NANOKVM_AUTHORIZED_KEYS="$HOME/.ssh/picoclaw-sd.pub" \
NANOKVM_WIFI_CONFIG="$PWD/wifi.conf" \
  nix build --impure .#boards.picoclaw.mainline.sd.c906l-lcd
```

This deliberately puts the Wi-Fi configuration into the Nix store and SD
image.  Treat that card and its build store as holding the credential; use a
dedicated installation SSID or prefer USB-first provisioning for a durable
credential.

For USB-first provisioning, omit `NANOKVM_WIFI_CONFIG`.  The image then waits
quietly for `/etc/wpa_supplicant/wpa_supplicant-wlan0.conf` instead of
restart-looping.  Connect via USB SSH as below, create that root-readable-only
file, and start the service:

```sh
install -d -m 0700 /etc/wpa_supplicant
umask 077
wpa_passphrase 'example-ssid' 'example-passphrase' \
  > /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
systemctl start wpa_supplicant-wlan0.service
networkctl status wlan0
```

`wpa_passphrase` writes a comment containing the original passphrase.  Remove
that comment if the file will be shared.  The file remains on the writable SD
root across reboots.

## Write and boot

Inspect the target device carefully; this command overwrites the selected SD
card.  Substitute the whole card device, not a partition.

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,RM
sudo dd if=result/sd-image/*.img of=/dev/mmcblkX bs=16M conv=fsync status=progress
sync
```

Insert the card, connect the PicoClaw USB-C data port to the workstation, and
power the board.  The stage-2 USB gadget is CDC-ECM with static addresses:

```sh
sudo ip link set usb0 up
sudo ip address replace 10.55.0.2/24 dev usb0
ssh root@10.55.0.1
```

Use the interface name chosen by the host if it is not `usb0`.  The watchdog is
enabled but does not consider unplugging USB a failure on this persistent
image.

After login, check that the firmware contract and DRM device are present, then
exercise both display and networking:

```sh
sg2002-c906l-ctl check
sg2002-c906l-drm-test /dev/dri/card0 32
networkctl status wlan0
```

The DRM test draws changing checkerboards, waits for page-flip completion, and
restores the previous mode.  Its completion establishes the Linux-to-C906L
path; inspect the panel itself for colour/orientation confirmation.
