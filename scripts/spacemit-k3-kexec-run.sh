#!/bin/sh

set -eu

kexec_extra_flags=

while [ "$#" -gt 0 ]; do
  case "$1" in
    --kexec-extra-flags)
      kexec_extra_flags="$2"
      shift
      ;;
  esac
  shift
done

init="@init@"
kernelParams="@kernelParams@"
script_dir=$(dirname "$(readlink -f "$0")")
initrd_tmp=$(TMPDIR="$script_dir" mktemp -d)

cleanup() {
  rm -rf "$initrd_tmp"
}
trap cleanup EXIT

ip="$script_dir/ip"
if [ ! -x "$ip" ]; then
  ip=$(command -v ip)
fi

gzip="$script_dir/gzip"
if [ ! -x "$gzip" ]; then
  gzip=$(command -v gzip)
fi

cd "$initrd_tmp"
mkdir -p ssh

extract_pub_keys() {
  home="$1"
  for file in .ssh/authorized_keys .ssh/authorized_keys2; do
    key="$home/$file"
    if [ -e "$key" ]; then
      grep -o '\(\(ssh\|ecdsa\|sk\)-[^ ]* .*\)' "$key" >> ssh/authorized_keys || true
    fi
  done
}

extract_pub_keys /root

if [ -n "${DOAS_USER-}" ]; then
  SUDO_USER="$DOAS_USER"
fi

if [ -n "${SUDO_USER-}" ]; then
  sudo_home=$(sh -c "echo ~$SUDO_USER")
  extract_pub_keys "$sudo_home"
fi

if [ -e /etc/ssh/authorized_keys.d/root ]; then
  cat /etc/ssh/authorized_keys.d/root >> ssh/authorized_keys
fi
if [ -n "${SUDO_USER-}" ] && [ -e "/etc/ssh/authorized_keys.d/$SUDO_USER" ]; then
  cat "/etc/ssh/authorized_keys.d/$SUDO_USER" >> ssh/authorized_keys
fi
for key in /etc/ssh/ssh_host_*; do
  [ -e "$key" ] || continue
  cp -a "$key" ssh
done

"$ip" --json addr > addrs.json
"$ip" -4 --json route > routes-v4.json
"$ip" -6 --json route > routes-v6.json

[ -f /etc/machine-id ] && cp /etc/machine-id machine-id

find . | "$script_dir/cpio" -o -H newc | "$gzip" -9 >> "$script_dir/initrd"

kexec_syscall_flags=
if printf '%s\n' "6.1" "$(uname -r)" | sort -c -V 2>&1; then
  kexec_syscall_flags=--kexec-syscall-auto
fi

if ! sh -c "'$script_dir/kexec' --load '$script_dir/bzImage' \
  $kexec_syscall_flags \
  $kexec_extra_flags \
  --initrd='$script_dir/initrd' --no-checks \
  --command-line 'init=$init $kernelParams'"
then
  echo "kexec failed, dumping dmesg" >&2
  dmesg | tail -n 100 >&2
  exit 1
fi

echo "machine will boot into nixos now..."
sync
# shellcheck disable=SC2086
"$script_dir/kexec" -e $kexec_extra_flags
