#!/usr/bin/env python3
"""postPatch for sg2002-cv181x-usb-dl.

Four fixes against the upstream Sipeed package:

1. `cv_usb_pyserial.py` opens the serial device with `timeout=10000`
   (ten THOUSAND seconds). When the CV181x ROM disconnects mid-read
   (~1 s after each enumeration cycle in USB-DL mode), pyserial
   blocks for ~2.8 hours on the next read. We change it to
   `timeout=1.0` so each read fails fast and the script can be
   retried from the next ROM cycle.

2. Its redundant 100 ms delay before opening the newly enumerated ACM
   device wastes a material part of the ROM's short connection window.

3. The sender calls pyserial's deprecated `flushOutput()` immediately
   after each write. Despite its name, that is an alias for
   `reset_output_buffer()` and discards bytes which have not reached the
   device yet. Replace it with `flush()`, which waits for queued bytes to
   drain before reading the acknowledgement.

4. A failed packet is retried forever inside `usb_send_chunk()`, so the
   outer recovery loop cannot start a clean downloader process until its
   coarse timeout kills the child. Limit each packet to three attempts
   and raise an error so the outer loop can retry promptly.

Idempotent — all fixes are skipped if their marker text is present.
"""
import re
import sys


MARKER = "# patched by sg2002-cv181x-rom-dl-skip-2nd-stage.py"
TIMEOUT_MARKER = "# patched timeout: short read so disconnect doesn't deadlock"
FAST_OPEN_MARKER = "# patched fast-open: skip the 100ms pre-open sleep"
FLUSH_DRAIN_MARKER = "# patched flush: drain queued bytes instead of discarding them"
BOUNDED_RETRY_MARKER = "# patched retry: bound a dead USB packet"


def main():
    if len(sys.argv) < 2:
        sys.exit(f"usage: {sys.argv[0]} <cv181x_rom_usb_download.py> [<cv_usb_pyserial.py>]")
    rom_dl_path = sys.argv[1]
    # The pyserial timeout patch lives next to cv_usb_pyserial.py.
    import os
    pyserial_path = os.path.join(
        os.path.dirname(rom_dl_path), "cv_usb_util", "cv_usb_pyserial.py"
    )
    patch_pyserial_timeout(pyserial_path)
    patch_pyserial_fast_open(pyserial_path)
    patch_pyserial_flush(pyserial_path)
    patch_pyserial_bounded_retry(pyserial_path)
    # NOTE: skip_2nd_stage is intentionally disabled. After BREAK the
    # chip transitions to FSBL which presents `cvi_utask` at 3346:1001
    # — FSBL uses that to pull the REST of FIP from the host (only
    # the first 4 KB went via ROM USB-DL). Skipping the 2nd-stage
    # push leaves FSBL with no rest-of-FIP, which is fatal.
    #
    # The wrapper relies on the per-attempt subprocess timeout to
    # bound rom-dl when 3346:1001 doesn't materialise (e.g. FSBL
    # crashes on DDR init). It will then proceed to poll fastboot
    # anyway in case U-Boot did make it up another way.
    # patch_skip_2nd_stage(rom_dl_path)
    _ = rom_dl_path  # keep arg used


def patch_pyserial_timeout(path):
    with open(path) as f:
        src = f.read()
    if TIMEOUT_MARKER in src:
        print(f"already patched (pyserial timeout): {path}")
        return
    pattern = re.compile(
        r'(self\.device\s*=\s*serial\.Serial\()timeout=10000(,\s*writeTimeout=[0-9.]+\))'
    )
    new_src, n = pattern.subn(
        lambda m: m.group(1) + "timeout=1.0" + m.group(2) +
                  f"  {TIMEOUT_MARKER}",
        src, count=1,
    )
    if n == 0:
        print(f"WARN: pyserial-timeout pattern not found in {path}; "
              f"continuing without that patch")
        return
    with open(path, "w") as f:
        f.write(new_src)
    print(f"patched (pyserial timeout): {path}")


def patch_pyserial_fast_open(path):
    """Upstream pyserial wrapper does:

        self.device = serial.Serial(timeout=..., writeTimeout=0.5)
        self.device.port = element.device
        ...
        time.sleep(0.1)
        self.device.close()
        connect = -1
        while connect == -1:
            try:
                self.device.open()
                ...

    The CV181x ROM holds /dev/ttyACM0 live for ~1 second per
    enumeration cycle. The pre-open `time.sleep(0.1)` plus the
    redundant `close()` of an unopened serial burns ~150ms of that
    window before we even try to open(). On a slow open() that's
    fatal. Skip both — Serial(...) is unopened by default, and we
    don't need the sleep.
    """
    with open(path) as f:
        src = f.read()
    if FAST_OPEN_MARKER in src:
        print(f"already patched (fast-open): {path}")
        return
    # Anchor on the two adjacent lines we want to drop.
    pattern = re.compile(
        r'(\n        time\.sleep\(0\.1\)\n        self\.device\.close\(\)\n)',
    )
    new_src, n = pattern.subn(
        f"\n        {FAST_OPEN_MARKER}\n",
        src, count=1,
    )
    if n == 0:
        print(f"WARN: fast-open pattern not found in {path}; skipping")
        return
    with open(path, "w") as f:
        f.write(new_src)
    print(f"patched (fast-open): {path}")


def patch_pyserial_flush(path):
    """serial_write() does:

        try:
            self.device.write(command)
            self.device.flushOutput()
        except serial.SerialTimeoutException as e:
            return pkt.FAIL

    pyserial implements flushOutput() as reset_output_buffer(), which
    uses tcflush(TCOFLUSH) to abort and discard queued output. That races
    the USB ACM transport: on a slower path the command can be discarded
    before it reaches the ROM, so recv_ack reads nothing forever. Use
    flush() instead; it calls tcdrain() and waits for the command to leave
    the host before we read its acknowledgement. A disconnect during the
    drain is still a transient failed attempt, so keep it bounded by the
    existing broad exception handling.
    """
    with open(path) as f:
        src = f.read()
    if FLUSH_DRAIN_MARKER in src:
        print(f"already patched (flush-drain): {path}")
        return
    pattern = re.compile(
        r'(\n            self\.device\.write\(command\)\n)'
        r'            self\.device\.flushOutput\(\)\n'
    )
    # A disconnect during tcdrain can raise termios.error, which is not
    # an OSError subclass on all supported Python versions.
    replacement = (
        r'\1'
        f'            try:  {FLUSH_DRAIN_MARKER}\n'
        '                self.device.flush()\n'
        '            except Exception:\n'
        '                pass\n'
    )
    new_src, n = pattern.subn(replacement, src, count=1)
    if n == 0:
        print(f"WARN: flush-drain pattern not found in {path}; skipping")
        return

    # serial_write also catches only `serial.SerialTimeoutException` on
    # both the write() and the recv_ack read(). Over a hub the device
    # cycles mid-2nd-stage push and read() raises the *parent*
    # serial.SerialException ("device reports readiness to read but
    # returned no data") — not a Timeout subclass — so it crashes too.
    # Broaden every SerialTimeoutException handler in the file to
    # Exception so any transient cycle just fails that chunk (-> the
    # caller retries) instead of killing the whole push.
    new_src = new_src.replace(
        "except serial.SerialTimeoutException as e:",
        "except Exception as e:  # broadened: hub cycles raise non-Timeout errors",
    )

    with open(path, "w") as f:
        f.write(new_src)
    print(f"patched (flush-drain + broadened serial excepts): {path}")


def patch_pyserial_bounded_retry(path):
    """Let the outer process-level retry recover from a dead ACM session.

    usb_send_chunk() currently rewinds after any failed acknowledgement
    and retries the same packet forever. Three packet attempts already
    span up to three serial read timeouts; after that, a fresh device
    query and file handle are safer than continuing on a dead endpoint.
    Raising is intentional because the callers ignore this method's
    return value.
    """
    with open(path) as f:
        src = f.read()
    if BOUNDED_RETRY_MARKER in src:
        print(f"already patched (bounded retry): {path}")
        return

    function_start = src.find("    def usb_send_chunk(")
    function_end = src.find("\n    def ", function_start + 1)
    if function_start == -1 or function_end == -1:
        sys.exit("FAILED to locate usb_send_chunk() for bounded retry")

    prefix = src[:function_start]
    body = src[function_start:function_end]
    suffix = src[function_end:]
    init_old = (
        "            last_pos = content_file.tell()\n"
        "            # print(\"Send to address 0x%x\" % dest_addr)\n"
    )
    init_new = (
        "            last_pos = content_file.tell()\n"
        f"            packet_retries = 0  {BOUNDED_RETRY_MARKER}\n"
        "            # print(\"Send to address 0x%x\" % dest_addr)\n"
    )
    result_old = (
        "                if send_ok == 0:\n"
        "                    dest_addr += tx_len - pkt.HEADER_SIZE\n"
        "                    content_size -= tx_len - pkt.HEADER_SIZE\n"
        "                else:\n"
        "                    last_pos -= tx_len - pkt.HEADER_SIZE\n"
    )
    result_new = (
        "                if send_ok == 0:\n"
        "                    packet_retries = 0\n"
        "                    dest_addr += tx_len - pkt.HEADER_SIZE\n"
        "                    content_size -= tx_len - pkt.HEADER_SIZE\n"
        "                else:\n"
        "                    packet_retries += 1\n"
        "                    if packet_retries >= 3:\n"
        "                        raise RuntimeError(\"USB packet failed after 3 attempts\")\n"
        "                    last_pos -= tx_len - pkt.HEADER_SIZE\n"
    )
    init_count = body.count(init_old)
    result_count = body.count(result_old)
    if init_count != 1 or result_count != 1:
        sys.exit(
            "FAILED to add bounded usb_send_chunk retry; "
            f"init matches={init_count}, result matches={result_count}"
        )
    body = body.replace(init_old, init_new, 1).replace(result_old, result_new, 1)
    new_src = prefix + body + suffix

    with open(path, "w") as f:
        f.write(new_src)
    print(f"patched (bounded retry): {path}")


def patch_skip_2nd_stage(path):
    with open(path) as f:
        src = f.read()
    if MARKER in src:
        print(f"already patched (skip-2nd-stage): {path}")
        return

    # The structure after BREAK is something like:
    #     cv_usb_serial.usb_send_req_data(pkt.CV_USB_BREAK, ...)
    #     print("break")
    #
    #     is_uboot_sent = False
    #     while True:
    #         del cv_usb_serial
    #         ...
    #
    # Replace from the `is_uboot_sent` line through the end of `main()`
    # with a clean exit. Anchoring on the surrounding text rather than
    # whitespace so reformatting upstream doesn't break us.

    pattern = re.compile(
        r'(\s*print\("break"\)\n)'                # group 1: anchor
        r'(.*?)'                                  # group 2: body to drop
        r'(\nif __name__ == ["\']__main__["\']:)',  # group 3: module trailer
        re.DOTALL,
    )

    def replacement(m):
        anchor, _body, trailer = m.group(1), m.group(2), m.group(3)
        new = (
            anchor +
            f"\n    {MARKER}\n"
            "    # mainline U-Boot has no cvi_utask; rom-dl's 2nd-stage\n"
            "    # poll loop hangs forever waiting for it. Exit cleanly\n"
            "    # so the wrapper can proceed to fastboot enumeration.\n"
            "    sys.exit(0)\n"
            + trailer
        )
        return new

    new_src, n = pattern.subn(replacement, src, count=1)
    if n == 0:
        sys.exit(
            f"FAILED to locate `print(\"break\")` ... `if __name__ == \"__main__\":`\n"
            f"in {path}. Did upstream change shape? Update this patch."
        )

    with open(path, "w") as f:
        f.write(new_src)
    print(f"patched: {path}")


if __name__ == "__main__":
    main()
