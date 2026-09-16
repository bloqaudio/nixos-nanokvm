#!/usr/bin/env python3
"""Host-side invariants for the exact-contract attach-only remoteproc."""

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


def main() -> None:
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    start = source.index("static void sg2002_mbox_receive(")
    end = source.index("static int sg2002_attach(", start)
    harness = Path(__file__).with_name("test_notify.c").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="c906l-notify-test-") as temporary:
        test_source = Path(temporary) / "test_notify.c"
        executable = Path(temporary) / "test_notify"
        test_source.write_text(harness.replace("/* @NOTIFY_FUNCTION@ */", source[start:end]),
                               encoding="utf-8")
        subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", str(test_source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=10)
    contract = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    table = contract["rpmsg"]["resourceTable"]

    require(table["serializedSize"] == 88, "resource ABI changed")
    require(table["vringCount"] == 2, "unexpected vring count")
    required = (
        '#include "sg2002-c906l-kernel-contract.h"',
        '"sophgo,contract-sha256"',
        '"sophgo,contract-epoch"',
        '"sophgo,abi-version"',
        '"sophgo,expected-capabilities"',
        '"sophgo,dormant-capabilities"',
        '"sophgo,lease-mask"',
        '"sophgo,profile-id"',
        '"sophgo,manifest-flags"',
        "SG2002_C906L_CONTRACT_SHA256_BYTES",
        "SG2002_C906L_CAPABILITY_WIRE_WIDTH",
        "memchr_inv(manifest->reserved1",
        "SG2002_C906L_RSC_VDEV_NOTIFY_ID_INITIAL",
        "SG2002_C906L_RSC_GUEST_FEATURES_INITIAL",
        "SG2002_C906L_RSC_STATUS_INITIAL",
        "SG2002_C906L_VRING_NOTIFY_ID_INITIAL",
        "SG2002_C906L_VRING_PHYSICAL_ADDRESS_INITIAL",
        "*size = SG2002_C906L_RSC_TABLE_SERIALIZED_SIZE",
        "SG2002_C906L_ACTIVATION_STATE_DORMANT",
        "SG2002_C906L_ACTIVATION_STATE_ACTIVE",
        "SG2002_C906L_ACTIVATION_STATE_REJECTED",
        "SG2002_C906L_ACTIVATION_STATE_LEASE_FAULT",
        "SG2002_C906L_FLAG_ACTIVATION_REJECTED",
        "SG2002_C906L_FLAG_ACTIVATION_FAILED",
        "SG2002_C906L_MANIFEST_FLAG_RPMSG_WHILE_DORMANT",
        "memcmp(&before, &snapshot->status, sizeof(before))",
        "!attempts || !request_id",
        "attempts || request_id",
        ".attach = sg2002_attach",
        ".detach = sg2002_detach",
        ".stop = sg2002_stop_transport",
    )
    for token in required:
        require(token in source, f"missing remoteproc invariant: {token}")

    require(
        source.count("priv->shared + SG2002_C906L_MANIFEST_OFFSET") >= 2,
        "manifest is not sampled twice",
    )
    snapshot_start = source.index("static int sg2002_read_contract_snapshot")
    snapshot_end = source.index("static int sg2002_validate_resource_table", snapshot_start)
    snapshot_reader = source[snapshot_start:snapshot_end]
    require(snapshot_reader.count("rmb();") == 3,
            "status/manifest snapshot reads are not fully ordered")
    require(
        source.count("SG2002_C906L_RESOURCE_TABLE_REGION_OFFSET") >= 4,
        "resource table is not validated and served from the contract region",
    )
    require(
        "*size = SG2002_C906L_RESOURCE_TABLE_REGION_SIZE" not in source,
        "remoteproc is told the 4K reservation is serialized table data",
    )
    require(
        "le32_to_cpu(first.device_features) !=" in source,
        "resource features are accepted as a superset",
    )
    require(
        "struct sg2002_c906l_resource_snapshot first" in source
        and "memcmp(&first, &second, sizeof(first))" in source,
        "resource table does not have a stable exact snapshot",
    )

    # This transport is an observer of activation and has no core lifecycle
    # authority. Its only direct shared-memory write takes virtio offline.
    for forbidden in (
        "SG2002_C906L_OP_ACTIVATE_LEASES",
        "sg2002_c906l_activation_request",
        "reset_control_",
        ".start =",
        ".load =",
    ):
        require(forbidden not in source, f"attach-only boundary violated: {forbidden}")
    writes = re.findall(r"\b(?:write[bwlq]|memcpy_toio)\s*\(", source)
    require(writes == ["writeb("], f"unexpected remoteproc MMIO writes: {writes}")
    require("offsetof(struct sg2002_c906l_resource_snapshot, status)" in source,
            "the sole write is not the virtio status byte")

    detach_start = source.index("static int sg2002_detach")
    detach_end = source.index("static int sg2002_stop_transport", detach_start)
    detach = source[detach_start:detach_end]
    require(detach.index("wmb();") < detach.index("mbox_send_message"),
            "detach doorbell can pass the restored clean resource table")
    unregister_start = source.index("static void sg2002_unregister_rproc")
    unregister_end = source.index("static ssize_t transport_stats_show", unregister_start)
    unregister = source[unregister_start:unregister_end]
    require("rproc_resource_cleanup" not in unregister,
            "remove duplicates remoteproc core resource cleanup")


if __name__ == "__main__":
    main()
