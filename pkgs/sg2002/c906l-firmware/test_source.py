#!/usr/bin/env python3
"""Host-side invariants for C906L mailbox hardware-spinlock coverage."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_span(source: str, name: str) -> tuple[int, int]:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = match.start()
    opening = source.index("{", match.start(), match.end())
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return start, offset + 1
    raise AssertionError(f"unterminated function {name}")


def main() -> None:
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    contract = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    mailbox = contract["soc"]["mailbox"]
    hwspin = mailbox["hardwareSpinlock"]

    require(hwspin["mailboxField"] == 4, "SPIN_MBOX is not field 4")
    require(
        mailbox["address"]
        + hwspin["registerOffset"]
        + hwspin["mailboxField"] * hwspin["registerStride"]
        == 0x019000D0,
        "SPIN_MBOX register address changed",
    )
    require(
        (hwspin["linuxTokenShift"], hwspin["c906lTokenShift"], hwspin["tokenWidth"])
        == (0, 8, 8),
        "cross-core token namespaces changed",
    )
    require(
        mailbox["channels"] == {"control": 0, "vqKick": 1, "vqNotify": 2},
        "mailbox channel allocation changed",
    )
    require(mailbox["processorCount"] == 4, "mailbox processor count changed")

    for token in (
        "SG2002_C906L_MAILBOX_HWSPIN_ADDRESS",
        "SG2002_C906L_MAILBOX_HWSPIN_C906L_TOKEN_SHIFT",
        "SG2002_C906L_MAILBOX_HWSPIN_TASK_ACQUIRE_ATTEMPTS",
        "SG2002_C906L_MAILBOX_HWSPIN_IRQ_ACQUIRE_ATTEMPTS",
        "SG2002_C906L_MAILBOX_HWSPIN_IRQ_CONSECUTIVE_DEFERRAL_LIMIT",
        "c906l_local_irq_save",
        "mailbox_lock_failures++",
        "mailbox_irq_lock_deferrals++",
        "c906l_mailbox_lock_failures",
        "c906l_unexpected_mailbox_events",
        "processor < SG2002_C906L_MAILBOX_PROCESSOR_COUNT",
        "Drain protocol-invalid channels without ever interpreting slots",
    ):
        require(token in source, f"missing mailbox-lock invariant: {token}")
    require("taskENTER_CRITICAL" not in source, "scheduler-only critical section remains")
    require(
        "SG2002_C906L_TIMER4\n" not in source,
        "legacy out-of-band Timer4 build define remains",
    )
    timer_guard = """#if defined(SG2002_C906L_HAVE_TIMER4) || \\
\tdefined(SG2002_C906L_HAVE_TIMER5) || \\
\tdefined(SG2002_C906L_HAVE_TIMER6) || \\
\tdefined(SG2002_C906L_HAVE_TIMER7)"""
    require(
        source.count(timer_guard) == 3,
        "timer validation, declaration, and plumbing are not guarded by all leases",
    )

    peripheral_leases = contract["peripheralLeases"]
    for peripheral_name, lease in peripheral_leases.items():
        match = re.fullmatch(r"timer([4-7])", peripheral_name)
        require(match is not None, f"unsupported C IRQ lease: {peripheral_name}")
        channel = int(match.group(1))
        require(
            lease["kind"] == "dw-apb-timer-channel"
            and lease["bank"]["channel"] == channel,
            f"{peripheral_name} is not the expected timer channel",
        )
        macro = f"SG2002_C906L_TIMER{channel}_IRQ"
        have_macro = f"SG2002_C906L_HAVE_TIMER{channel}"
        trampoline = f"timer{channel}_isr"
        require(
            source.count(f"#ifdef {have_macro}\n\tcase {channel}U:") == 2,
            f"{peripheral_name} install/disable cases are not selection-guarded",
        )
        for token in (
            f"#ifdef {have_macro}",
            f"#if TIMER_INTR_{channel} != {macro}",
            f"static int {trampoline}(int irqn, void *priv)",
            f"if ((uint32_t)irqn != {macro} || priv != NULL)",
            f"return c906l_timer_interrupt({channel}U, {macro});",
            f"return request_irq({macro}, {trampoline}, 0,",
            f"disable_irq({macro});",
        ):
            require(token in source, f"missing {peripheral_name} IRQ invariant: {token}")

    if peripheral_leases:
        for token in (
            "c906l_timer_interrupt(uint32_t channel, uint32_t irq)",
            "c906l_timer_irq_install(uint32_t channel, uint32_t irq)",
            "c906l_timer_irq_disable(uint32_t channel, uint32_t irq)",
            "mask/disable that channel",
            "complete the PLIC claim",
        ):
            require(token in source, f"missing generic timer IRQ invariant: {token}")

        install_start, install_end = function_span(source, "c906l_timer_irq_install")
        install = source[install_start:install_end]
        disable_start, disable_end = function_span(source, "c906l_timer_irq_disable")
        disable = source[disable_start:disable_end]
        require(
            "switch (channel)" in install and "switch (channel)" in disable,
            "generic timer IRQ operations do not dispatch by channel",
        )
        require(
            "request_irq(" not in install[: install.index("switch (channel)")]
            and "disable_irq(" not in disable[: disable.index("switch (channel)")],
            "timer IRQ operation has a side effect before channel validation",
        )
        require(
            re.search(r"default:\s*return -1;", install) is not None
            and re.search(r"default:\s*return;", disable) is not None,
            "generic timer IRQ operations do not reject unselected channels",
        )
        for peripheral_name, lease in peripheral_leases.items():
            channel = lease["bank"]["channel"]
            macro = f"SG2002_C906L_TIMER{channel}_IRQ"
            for operation, body, rejection in (
                ("install", install, "return -1;"),
                ("disable", disable, "return;"),
            ):
                case = re.search(
                    rf"case {channel}U:\s*if \(irq != {macro}\)\s*"
                    rf"{re.escape(rejection)}",
                    body,
                )
                require(
                    case is not None,
                    f"{operation} does not reject a mismatched {peripheral_name} IRQ",
                )

    protected = {
        name: function_span(source, name)
        for name in ("mailbox_isr", "c906l_platform_start", "mailbox_send")
    }
    for name, (start, end) in protected.items():
        body = source[start:end]
        require(body.count("mailbox_lock_acquire(") == 1, f"{name} has no single acquire")
        require(body.count("mailbox_lock_release(") == 1, f"{name} has no single release")

    operations = (
        r"\bslots\s*\[",
        r"\.mbox_info\s*[&|]=",
        r"\.mbox_info\s*&\s*bit",
        r"\.mbox_set\.mbox_set\s*=",
        r"\.cpu_mbox_int_clr\.mbox_int_clr\s*=",
        r"\.cpu_mbox_int_int\.mbox_int\s*;",
    )
    for pattern in operations:
        for operation in re.finditer(pattern, source):
            owner = next(
                (
                    (name, start, end)
                    for name, (start, end) in protected.items()
                    if start <= operation.start() < end
                ),
                None,
            )
            require(owner is not None, f"unlocked mailbox MMIO operation: {operation.group()}")
            name, start, end = owner
            body_before = source[start : operation.start()]
            body_after = source[operation.end() : end]
            require(
                "mailbox_lock_acquire(" in body_before
                and "mailbox_lock_release(" in body_after,
                f"{name} mailbox operation lies outside acquire/release",
            )

    irq_start, irq_end = protected["mailbox_isr"]
    irq = source[irq_start:irq_end]
    require(
        "mailbox_irq_lock_deferrals++" in irq
        and "mailbox_irq_lock_streak <" in irq
        and "mailbox_irq_lock_streak ==" in irq
        and "mailbox_irq_lock_streak = 0" in irq
        and "disable_irq(SG2002_C906L_MAILBOX_C906L_IRQ)" in irq,
        "IRQ lock exhaustion does not retry then fail closed at its consecutive bound",
    )
    require(
        irq.index("mailbox_lock_failures++")
        < irq.index("disable_irq(SG2002_C906L_MAILBOX_C906L_IRQ)"),
        "terminal IRQ lock failure is not published before masking",
    )
    unexpected = irq[irq.index("if (unexpected != 0U)") : irq.index(
        "lock_result = mailbox_lock_release"
    )]
    require("slots[" not in unexpected, "unexpected channel payload was interpreted")
    require(
        len(re.findall(r"\.cpu_mbox_int_clr\.mbox_int_clr\s*=", unexpected)) == 1
        and len(re.findall(r"\.mbox_info\s*&=", unexpected)) == 1,
        "unexpected channels are not drained exactly once",
    )


if __name__ == "__main__":
    main()
