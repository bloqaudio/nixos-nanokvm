#!/usr/bin/env python3
"""Host-side invariants for the exact-contract Linux control driver."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_text(source: str, start: str, end: str) -> str:
    return source[source.index(start) : source.index(end, source.index(start))]


def c_strings(source: str) -> str:
    return "".join(re.findall(r'"((?:\\.|[^"\\])*)"', source))


def main() -> None:
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    handoff = Path(sys.argv[3]).read_text(encoding="utf-8")
    read_function = function_text(source, "static ssize_t sg2002_c906l_read(",
                                  "static __poll_t sg2002_c906l_poll(")
    harness = Path(__file__).with_name("test_read.c").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="c906l-read-test-") as temporary:
        test_source = Path(temporary) / "test_read.c"
        executable = Path(temporary) / "test_read"
        test_source.write_text(harness.replace("/* @READ_FUNCTION@ */", read_function),
                               encoding="utf-8")
        subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", str(test_source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)
    contract = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    abi = contract["abi"]
    activation = contract["activation"]

    require((abi["major"], abi["minor"]) == (1, 1), "wrong control ABI")
    require(activation["manifest"]["size"] == 128, "wrong manifest size")
    require(activation["request"]["size"] == 128, "wrong request size")
    require(activation["linuxResponseTimeoutMs"] == 2000, "timeout changed")
    require(
        activation["request"]["fields"][-1]
        == {"name": "commit", "offset": 124, "width": 4},
        "activation commit is not last",
    )

    required = (
        '#include "sg2002-c906l-kernel-contract.h"',
        '#include "picoclaw-lcd-handoff.h"',
        '"sophgo,contract-sha256"',
        '"sophgo,contract-epoch"',
        '"sophgo,abi-version"',
        '"sophgo,expected-capabilities"',
        '"sophgo,dormant-capabilities"',
        '"sophgo,lease-mask"',
        '"sophgo,profile-id"',
        '"sophgo,manifest-flags"',
        '"sophgo,activation-required"',
        "SG2002_C906L_CONTRACT_SHA256_BYTES",
        "memchr_inv(manifest->reserved1",
        "SG2002_C906L_LEASE_WIRE_WIDTH",
        "SG2002_C906L_CAPABILITY_WIRE_WIDTH",
        "SG2002_C906L_ACTIVATION_RESPONSE_TIMEOUT_MS",
        "SG2002_ACTIVATION_RECOVERED",
        "sysfs_create_group",
        "devm_ioremap_wc(",
        "u64 tx_word;",
        "ctl->client.tx_done = sg2002_c906l_txdone",
        "mbox_send_message(ctl->channel, &ctl->tx_word)",
        "memcmp(&before, &snapshot->status, sizeof(before))",
        "le32_to_cpu(initial->status.generation)",
        "ctl->activation_sequence = request_id & 0xffff",
        "(attempts || request_id)",
        "(!attempts || !request_id)",
        "SG2002_C906L_ACTIVATION_STATE_REJECTED",
        "SG2002_C906L_FLAG_ACTIVATION_REJECTED",
    )
    for token in required:
        require(token in source, f"missing control invariant: {token}")

    require(
        source.count("ctl->shared + SG2002_C906L_MANIFEST_OFFSET") >= 2,
        "manifest is not sampled twice",
    )
    require(
        source.count("mbox_send_message(ctl->channel, &ctl->tx_word)") == 2,
        "all control sends must use the persistent device-owned TX word",
    )
    require(
        "An accepted send is released exclusively by tx_done" in source,
        "activation error cleanup may reuse TX storage before tx_done",
    )
    require(
        re.search(r"mbox_send_message\([^;]*&(request|word)\)", source) is None,
        "a stack mailbox message escaped its lifetime",
    )
    require(
        re.search(
            r"message\.opcode\s*==\s*SG2002_C906L_OP_ACTIVATE_LEASES\)\s*"
            r"return\s+-EPERM;",
            source,
        )
        is not None,
        "raw userspace can submit activation",
    )

    writer = source.index("static void sg2002_write_activation_request")
    writer_end = source.index("static int sg2002_activation_terminal", writer)
    body = source[writer:writer_end]
    invalidate = body.index("writel(0")
    publish = body.index("memcpy_toio")
    commit = body.index("writel(SG2002_C906L_ACTIVATION_REQUEST_COMMIT")
    require(invalidate < publish < commit, "activation record is not commit-last")
    require(body.count("wmb();") >= 3, "activation I/O ordering is incomplete")

    terminal = function_text(
        source, "static int sg2002_activation_terminal", "static int sg2002_activate_leases"
    )
    require(
        "!got_response && !allow_status_recovery" in terminal,
        "status recovery does not quarantine a possibly late activation response",
    )
    require(
        terminal.index("got_response && response_result")
        < terminal.index("SG2002_C906L_ACTIVATION_STATE_ACTIVE"),
        "a non-success activation response does not win over ACTIVE status",
    )
    activator = function_text(
        source, "static int sg2002_activate_leases", "static int sg2002_c906l_open"
    )
    accepted_path = activator[activator.index("deadline = jiffies") :]
    require(
        "ctl->tx_pending = false" not in accepted_path,
        "an accepted activation send can release TX storage before tx_done",
    )

    snapshot_reader = function_text(
        source,
        "static int sg2002_read_contract_snapshot",
        "static int sg2002_validate_activation_source",
    )
    require(snapshot_reader.count("rmb();") == 3, "snapshot reads are not fully ordered")
    require(
        "memcmp(&before, &snapshot->status, sizeof(before))" in snapshot_reader,
        "full status snapshots are not compared",
    )

    reader = function_text(
        source, "static ssize_t sg2002_c906l_read", "static __poll_t sg2002_c906l_poll"
    )
    require(
        "!READ_ONCE(ctl->tx_pending) && READ_ONCE(ctl->tx_status) < 0" in reader,
        "a failed asynchronous raw send can leave read blocked forever",
    )

    # The generated header owns protocol values; the driver may only define a
    # derived response opcode and a local close timeout.
    handwritten = re.findall(r"^#define\s+SG2002_C906L_(\w+)", source, re.MULTILINE)
    require(
        set(handwritten) <= {"CLOSE_MS", "ACTIVATE_RESPONSE"},
        f"handwritten protocol constants found: {handwritten}",
    )
    for forbidden in ("writel.*RESET", "rproc_boot"):
        require(re.search(forbidden, source) is None, f"unsafe operation: {forbidden}")

    # The board hook is opt-in through the dedicated DT and is called only
    # after the exact live manifest read.  A non-default pinctrl state prevents
    # the driver core from touching pads before probe performs that check.
    probe = function_text(source, "static int sg2002_c906l_probe", "static void sg2002_c906l_remove")
    require(
        probe.index("sg2002_read_contract_snapshot")
        < probe.index("sg2002_picoclaw_lcd_prepare")
        < probe.index("sg2002_activate_leases"),
        "PicoClaw board MMIO is not bracketed by validation and activation",
    )
    require(
        probe.count("__module_get(THIS_MODULE)") >= 3,
        "an activated PicoClaw lease can release retained board resources",
    )
    for token in (
        '"sophgo,picoclaw-lcd-handoff"',
        '"picoclaw-lcd-handoff"',
        '"sophgo,picoclaw-ephy-reg"',
        '"sophgo,picoclaw-pinmux-reg"',
        "devm_clk_get(dev, \"spi\")",
        "devm_clk_get(dev, \"pclk\")",
        "clk_rate_exclusive_get(clk)",
        "clk_rate_exclusive_put(data)",
        "devm_reset_control_get_exclusive(dev, \"spi\")",
        "devm_reset_control_get_exclusive(dev, \"gpio\")",
        "pinctrl_select_state",
        "0x804, 0x1, 0x1",
        "0x808, 0x1f, 0x1",
        "0x800, 0x4, 0x4",
        "0x07c, 0x1f00, 0x500",
        "0x078, 0xfff, 0xf00",
        "0x074, ~0U, 0x606",
        "0x070, ~0U, 0x606",
    ):
        require(token in handoff, f"missing PicoClaw handoff invariant: {token}")

    for field in (
        "profile=",
        "profile_id=",
        "sha256=",
        "epoch=",
        "abi=",
        "final_capabilities=",
        "dormant_capabilities=",
        "lease_mask=",
        "manifest_flags=",
        "activation_required=",
        "driver_outcome=",
        "state=",
        "error=",
        "attempts=",
        "request_id=",
    ):
        require(field in source, f"sysfs ABI omitted {field}")

    contract_show = function_text(
        source, "static ssize_t contract_show", "static DEVICE_ATTR_RO(contract)"
    )
    require(
        c_strings(contract_show)
        == "profile=%s profile_id=%u sha256=%s epoch=%u abi=%u.%u "
        "final_capabilities=0x%016llx dormant_capabilities=0x%016llx "
        "lease_mask=0x%016llx manifest_flags=0x%08x activation_required=%u\\n",
        "contract sysfs grammar changed",
    )
    require(
        re.search(
            r"SG2002_C906L_PROFILE_NAME,\s*SG2002_C906L_PROFILE_ID,\s*"
            r"SG2002_C906L_CONTRACT_SHA256,\s*SG2002_C906L_CONTRACT_EPOCH,",
            contract_show,
        )
        is not None,
        "contract sysfs arguments are duplicated or out of order",
    )
    activation_show = function_text(
        source, "static ssize_t activation_show", "static DEVICE_ATTR_RO(activation)"
    )
    require(
        c_strings(activation_show)
        == "unavailable error=%d\\n"
        "driver_outcome=%s state=%u error=%u attempts=%u request_id=%u\\n",
        "activation sysfs grammar changed",
    )


if __name__ == "__main__":
    main()
