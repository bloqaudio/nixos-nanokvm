# Exercise the real RISC-V initrd userspace within the board's 256 MiB limit.
# QEMU models virtio, not SG2002 peripherals; only those hardware units differ.
{ pkgs, nixpkgs, board }:
let
  fixture = nixpkgs + "/nixos/tests/initrd-network-ssh";
  pthreadProbe = board.pkgs.runCommandCC "sg2002-pthread-cancel-probe" { } ''
    mkdir -p "$out/bin"
    $CC -O2 -Wall -Wextra -Werror -pthread ${./pthread-cancel.c} \
      -o "$out/bin/pthread-cancel-probe"
  '';
  testBoard = board.extendModules {
    modules = [ ({ lib, ... }: {
      boot.initrd.network.ssh.authorizedKeys = lib.mkForce [
        (lib.fileContents (fixture + "/id_ed25519.pub"))
      ];
      sg2002.initrd.kernelModules = [ "virtio_mmio" "virtio_net" ];
      sg2002.watchdogKeeper.initrd.enable = lib.mkForce false;
      sg2002.wifi.wpaConf = lib.mkForce null;
      boot.initrd.systemd.extraBin.pthread-cancel-probe =
        "${pthreadProbe}/bin/pthread-cancel-probe";
      boot.initrd.systemd.services = {
        usb-gadget.enable = lib.mkForce false;
      };
    }) ];
  };
  cfg = testBoard.config;
in pkgs.runCommand "sg2002-initrd-boot" {
  nativeBuildInputs = [ pkgs.qemu pkgs.openssh pkgs.coreutils pkgs.gnugrep ];
} ''
  # A successful reload must not be obtained by disabling verification.
  grep -qx 'CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=y' ${cfg.boot.kernelPackages.kernel.configfile}
  grep -qx 'CONFIG_CFG80211_USE_KERNEL_REGDB_KEYS=y' ${cfg.boot.kernelPackages.kernel.configfile}
  cp ${fixture + "/id_ed25519"} key
  chmod 600 key
  qemu-system-riscv64 -machine virt -cpu thead-c906 -m 256 -smp 1 \
    -nographic -monitor none -no-reboot \
    -kernel ${cfg.system.build.kernel}/Image \
    -initrd ${cfg.system.build.initialRamdisk}/initrd \
    -append 'console=ttyS0,115200 earlycon=uart8250,mmio,0x10000000 panic=10 rdinit=/init' \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-device,netdev=net0 > console.log 2>&1 &
  vm=$!
  trap 'kill "$vm" 2>/dev/null || true' EXIT
  ssh_test() {
    ssh -F /dev/null -i key -p 2222 -o BatchMode=yes -o IdentitiesOnly=yes \
      -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile="$PWD/known_hosts" \
      -o ConnectTimeout=3 root@127.0.0.1 "$@"
  }
  ready=0
  for attempt in $(seq 1 90); do
    if ssh_test true 2>/dev/null; then ready=1; break; fi
    kill -0 "$vm" || break
    sleep 2
  done
  if [ "$ready" != 1 ]; then cat console.log; exit 1; fi
  # A partially unpacked initrd can still reach SSH. Do not accept that as
  # a successful boot merely because the missing files were not used yet.
  if grep -E 'Initramfs unpacking failed|Kernel panic|Out of memory:' console.log; then
    cat console.log
    exit 1
  fi
  ssh_test '
    set -eu
    test -e /etc/initrd-release
    test ! -e /run/current-system
    test "$(cat /proc/1/comm)" = systemd
    systemctl is-active initrd.target sshd systemd-networkd systemd-resolved
    # QEMU has no WLAN: the real optional unit must not queue a missing
    # device or delay initrd.target. Do not hide that regression by masking it.
    test -z "$(systemctl show sys-subsystem-net-devices-wlan0.device -p Job --value)"
    test -z "$(systemctl --failed --no-legend --plain)"
    timeout 10 pthread-cancel-probe
    # The normal firmware collection is disabled on this small image. Check
    # the signed regulatory database through cfg80211, not just its pathname.
    # There is no radio in this VM: selecting a non-world domain proves the
    # database was loaded and accepted, without changing any hardware policy.
    modprobe cfg80211
    iw reg reload
    iw reg set US
    regulatory_ready=0
    for attempt in $(seq 1 20); do
      case "$(iw reg get)" in
        *"country US:"*) regulatory_ready=1; break ;;
      esac
      sleep 0.25
    done
    test "$regulatory_ready" = 1
    iw reg get
    iw reg set 00
    test "$(systemctl is-enabled initrd-switch-root.service)" = masked
    while read -r device target rest; do
      test "$target" != /sysroot
    done < /proc/mounts
    test -n "$(ip -4 -o address show scope global)"
    resolvectl query _gateway
    test -L /etc/resolv.conf
    ip -4 address show
    df -k /
    # Leave room for runtime state and diagnostics on the tmpfs root. A
    # compressed/raw archive limit cannot account for page rounding or the
    # memory available when the kernel initially sizes this filesystem.
    set -- $(df -k --output=avail /)
    test "$2" -ge 8192
    cat /proc/meminfo
  ' > evidence.txt
  # Reject a key not present in authorized_keys as well as accepting the fixture.
  ssh-keygen -q -t ed25519 -N "" -f wrong-key
  if ssh -F /dev/null -i wrong-key -p 2222 -o BatchMode=yes -o IdentitiesOnly=yes \
      -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$PWD/known_hosts" \
      root@127.0.0.1 true; then
    echo 'Unexpected authentication with unauthorized key' >&2
    exit 1
  fi
  mkdir "$out"
  cp console.log evidence.txt "$out/"
''
