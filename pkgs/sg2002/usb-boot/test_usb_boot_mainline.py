from dataclasses import replace
from pathlib import Path
import sys
import subprocess
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sg2002_c906l_contract as contract

from usb_boot_mainline import (
    C906LManifest,
    C906LBringupError,
    C906L_CAPABILITY_WIRE_WIDTH,
    C906L_CAP_SHMEM_HEARTBEAT,
    C906L_RESET_BIT,
    C906L_RESET_REG,
    C906L_SEC_ENABLE_BIT,
    C906L_SEC_SYS_REG,
    C906L_STATE_RUNNING,
    C906LStatus,
    C906LSnapshot,
    C906L_VECTOR_HIGH_REG,
    C906L_VECTOR_LOW_REG,
    apply_c906l_reset_sequence,
    arm_uboot_watchdog,
    bootm_handoff_commands,
    SG2002_WDT_BASE,
    decode_c906l_manifest,
    decode_c906l_snapshot,
    decode_c906l_status,
    parse_uboot_crc32,
    parse_uboot_words,
    select_c906l_handoff_mode,
    validate_c906l_snapshot,
    validate_c906l_snapshot_pair,
    validate_c906l_layout,
)


def valid_snapshot(generation=7, heartbeat=0x1122334455667788):
    capabilities = (
        contract.DORMANT_CAPABILITIES
        if contract.ACTIVATION_REQUIRED
        else contract.EXPECTED_CAPABILITIES
    )
    activation_state = (
        contract.ACTIVATION_STATE_DORMANT
        if contract.ACTIVATION_REQUIRED
        else contract.ACTIVATION_STATE_ACTIVE
    )
    return C906LSnapshot(
        status=C906LStatus(
            magic=contract.SHMEM_MAGIC,
            abi_major=contract.ABI_MAJOR,
            abi_minor=contract.ABI_MINOR,
            struct_size=contract.STATUS_SIZE,
            state=contract.STATE_RUNNING,
            generation=generation,
            flags=0,
            heartbeat=heartbeat,
            capabilities=capabilities,
            last_request=0,
            last_response=0,
            activation_state=activation_state,
            activation_error=contract.ACTIVATION_RESULT_SUCCESS,
            activation_attempts=0,
            activation_request_id=0,
        ),
        manifest=C906LManifest(
            magic=contract.MANIFEST_MAGIC,
            format_major=contract.MANIFEST_FORMAT_MAJOR,
            format_minor=contract.MANIFEST_FORMAT_MINOR,
            struct_size=contract.MANIFEST_SIZE,
            generation=generation,
            contract_epoch=contract.CONTRACT_EPOCH,
            profile_id=contract.PROFILE_ID,
            abi_major=contract.ABI_MAJOR,
            abi_minor=contract.ABI_MINOR,
            capability_width=C906L_CAPABILITY_WIRE_WIDTH,
            lease_width=contract.LEASE_WIRE_WIDTH,
            final_capabilities=contract.EXPECTED_CAPABILITIES,
            dormant_capabilities=contract.DORMANT_CAPABILITIES,
            lease_mask=contract.LEASE_MASK,
            flags=contract.MANIFEST_FLAGS,
            reserved0=0,
            contract_sha256=bytes.fromhex(contract.CONTRACT_SHA256),
            reserved1=bytes(28),
            commit=contract.MANIFEST_COMMIT,
        ),
    )


class WatchdogTests(unittest.TestCase):
    def test_arm_order_and_readback(self):
        registers = {}
        writes = []

        def write(address, value):
            writes.append((address, value))
            registers[address] = value

        arm_uboot_watchdog(registers.__getitem__, write)
        self.assertEqual(writes, [
            (SG2002_WDT_BASE + 4, 0xff),
            (SG2002_WDT_BASE + 12, 0x76),
            (SG2002_WDT_BASE, 1),
        ])

    def test_fail_closed_if_either_register_does_not_confirm(self):
        for control, timeout in [(0, 0xff), (2, 0xff), (3, 0xff),
                                 (0x41, 0xff), (0x81, 0xff), (1, 0x7f)]:
            registers = {SG2002_WDT_BASE: control,
                         SG2002_WDT_BASE + 4: timeout}
            with self.subTest(control=control, timeout=timeout):
                with self.assertRaises(C906LBringupError):
                    arm_uboot_watchdog(registers.__getitem__, lambda *_: None)

    def test_register_failure_stops_without_further_writes(self):
        for failing_write in range(3):
            writes = []

            def write(address, value):
                writes.append((address, value))
                if len(writes) == failing_write + 1:
                    raise C906LBringupError('injected fastboot failure')

            with self.subTest(failing_write=failing_write):
                with self.assertRaises(C906LBringupError):
                    arm_uboot_watchdog(lambda _: self.fail('read after failed write'), write)
                self.assertEqual(len(writes), failing_write + 1)


class BootHandoffTests(unittest.TestCase):
    def test_legacy_fastboot_limit_and_lossless_bootargs(self):
        values = [
            '',
            'console=ttyS0,115200 earlycon=sbi panic=10 oops=panic ' * 8,
            'init=/nix/store/' + 'a' * 100 + '/init',
            'quoted="two words" literal=$value slash=\\ backtick=`true` utf8=é',
        ]
        for value in values:
            with self.subTest(value=value):
                setup, handoff = bootm_handoff_commands(value)
                for command in setup + [handoff]:
                    self.assertLessEqual(len(('oem run:' + command).encode()), 64)
                # Exercise quoting and expansion with POSIX shell semantics.
                script = 'setenv() { export "$1=${2-}"; }; bootargs=old;\n'
                script += '\n'.join(setup) + '\nprintf %s "$bootargs"'
                result = subprocess.check_output(['sh', '-c', script], text=True)
                self.assertEqual(result, value)
                self.assertTrue(handoff.endswith('bootm 82000000'))

    def test_no_bootargs_override_or_soft_disconnect(self):
        setup, handoff = bootm_handoff_commands(None, False)
        self.assertFalse(any('bootargs' in command for command in setup))
        self.assertEqual(handoff, 'bootm 82000000')

    def test_rejects_control_characters(self):
        for value in ['a\nb', 'a\rb', 'a\0b']:
            with self.assertRaises(ValueError):
                bootm_handoff_commands(value)


class UBootOutputTests(unittest.TestCase):
    def test_memory_dump_accepts_debian_fastboot_status_padding(self):
        output = (
            " " * 51 + "(bootloader) 03010000: 00000001                             ....\n"
            "OKAY [  0.000s]\nFinished. Total time: 0.000s\n"
        )
        self.assertEqual(parse_uboot_words(output, 0x03010000, 1), [1])

    def test_crc32_accepts_fastboot_console_prefix(self):
        output = (
            "(bootloader) crc32 for 88000000 ... 88002fff ==> 961ca69b\n"
            "OKAY [  0.001s]\n"
        )
        self.assertEqual(
            parse_uboot_crc32(output, 0x88000000, 0x3000),
            0x961CA69B,
        )

    def test_crc32_rejects_result_for_another_range(self):
        with self.assertRaisesRegex(C906LBringupError, "found 0"):
            parse_uboot_crc32(
                "CRC32 for 82000000 ... 820fffff ==> 12345678\n",
                0x88000000,
                0x3000,
            )

    def test_status_dump_decodes_little_endian_u64_fields(self):
        output = "\n".join([
            "(bootloader) 88100000: 4d564b4e 00010001 00000040 00000002",
            "(bootloader) 88100010: 00000007 00000000 55667788 11223344",
            "(bootloader) 88100020: 00000003 00000000 aabbccdd 12345678",
            "(bootloader) 88100030: 76543210 fedcba98 00000004 00000000",
        ])
        words = parse_uboot_words(output, 0x88100000, 16)
        status = decode_c906l_status(words)
        self.assertEqual(status.abi_major, 1)
        self.assertEqual(status.abi_minor, 1)
        self.assertEqual(status.state, C906L_STATE_RUNNING)
        self.assertEqual(status.generation, 7)
        self.assertEqual(status.heartbeat, 0x1122334455667788)
        self.assertEqual(status.capabilities, C906L_CAP_SHMEM_HEARTBEAT | 1)
        self.assertEqual(status.last_request, 0x12345678AABBCCDD)
        self.assertEqual(status.last_response, 0xFEDCBA9876543210)
        self.assertEqual(status.activation_state, 4)
        self.assertEqual(status.activation_error, 0)
        self.assertEqual(status.activation_attempts, 0)
        self.assertEqual(status.activation_request_id, 0)

    def test_status_dump_must_be_contiguous(self):
        with self.assertRaisesRegex(C906LBringupError, "omitted"):
            parse_uboot_words(
                "88100000: 4d564b4e 00000001 00000040 00000002\n",
                0x88100000,
                16,
            )

    def test_manifest_dump_decodes_digest_and_reserved_bytes(self):
        snapshot = valid_snapshot()
        manifest = snapshot.manifest
        words = [
            manifest.magic,
            manifest.format_major | manifest.format_minor << 16,
            manifest.struct_size,
            manifest.generation,
            manifest.contract_epoch,
            manifest.profile_id,
            manifest.abi_major | manifest.abi_minor << 16,
            manifest.capability_width | manifest.lease_width << 16,
            manifest.final_capabilities & 0xffffffff,
            manifest.final_capabilities >> 32,
            manifest.dormant_capabilities & 0xffffffff,
            manifest.dormant_capabilities >> 32,
            manifest.lease_mask & 0xffffffff,
            manifest.lease_mask >> 32,
            manifest.flags,
            manifest.reserved0,
        ]
        for offset in range(0, len(manifest.contract_sha256), 4):
            words.append(int.from_bytes(
                manifest.contract_sha256[offset:offset + 4], "little"
            ))
        words.extend([0] * 7)
        words.append(manifest.commit)
        self.assertEqual(decode_c906l_manifest(words), manifest)


class IdentityValidationTests(unittest.TestCase):
    def test_profile_has_exact_expected_pre_linux_activation_state(self):
        snapshot = valid_snapshot()
        validate_c906l_snapshot(snapshot)
        if contract.ACTIVATION_REQUIRED:
            self.assertEqual(
                snapshot.status.activation_state,
                contract.ACTIVATION_STATE_DORMANT,
            )
            self.assertEqual(
                snapshot.status.capabilities,
                contract.DORMANT_CAPABILITIES,
            )
        else:
            self.assertEqual(
                snapshot.status.activation_state,
                contract.ACTIVATION_STATE_ACTIVE,
            )
            self.assertEqual(
                snapshot.status.capabilities,
                contract.EXPECTED_CAPABILITIES,
            )

    def test_two_stable_snapshots_with_progress_are_accepted(self):
        first = valid_snapshot(heartbeat=10)
        second = valid_snapshot(heartbeat=11)
        self.assertEqual(validate_c906l_snapshot_pair(first, second), second)

    def test_torn_or_restarted_snapshot_pair_is_rejected(self):
        first = valid_snapshot(generation=7, heartbeat=10)
        second = valid_snapshot(generation=8, heartbeat=11)
        with self.assertRaisesRegex(C906LBringupError, "manifest changed"):
            validate_c906l_snapshot_pair(first, second)

    def test_malformed_manifest_is_rejected(self):
        snapshot = valid_snapshot()
        for field, value, message in (
            ("struct_size", contract.MANIFEST_SIZE - 4, "manifest size"),
            ("reserved0", 1, "reserved0"),
            ("reserved1", bytes(27) + b"\\x01", "reserved1"),
            ("commit", 0, "manifest commit"),
        ):
            with self.subTest(field=field):
                malformed = replace(
                    snapshot,
                    manifest=replace(snapshot.manifest, **{field: value}),
                )
                with self.assertRaisesRegex(C906LBringupError, message):
                    validate_c906l_snapshot(malformed)

    def test_contract_identity_mismatches_are_rejected(self):
        snapshot = valid_snapshot()
        mismatches = (
            ("contract_epoch", snapshot.manifest.contract_epoch + 1,
             "contract epoch"),
            ("profile_id", snapshot.manifest.profile_id ^ 1, "profile ID"),
            ("abi_minor", snapshot.manifest.abi_minor + 1,
             "manifest ABI minor"),
            ("capability_width", snapshot.manifest.capability_width - 1,
             "capability wire width"),
            ("lease_width", snapshot.manifest.lease_width - 1,
             "lease wire width"),
            ("final_capabilities", snapshot.manifest.final_capabilities ^ 1,
             "final capabilities"),
            ("dormant_capabilities",
             snapshot.manifest.dormant_capabilities ^ 1,
             "dormant capabilities"),
            ("lease_mask", snapshot.manifest.lease_mask ^ 1, "lease mask"),
            ("flags", snapshot.manifest.flags ^ 1, "manifest flags"),
            ("contract_sha256", bytes(32), "contract SHA-256"),
        )
        for field, value, message in mismatches:
            with self.subTest(field=field):
                mismatch = replace(
                    snapshot,
                    manifest=replace(snapshot.manifest, **{field: value}),
                )
                with self.assertRaisesRegex(C906LBringupError, message):
                    validate_c906l_snapshot(mismatch)

    def test_status_mismatch_and_activation_history_are_rejected(self):
        snapshot = valid_snapshot()
        mismatches = (
            ("abi_minor", snapshot.status.abi_minor + 1, "status ABI minor"),
            ("capabilities", snapshot.status.capabilities ^ 1,
             "status capabilities"),
            ("flags", 1, "status flags"),
            ("activation_error", 1, "activation error"),
            ("activation_attempts", 1, "activation attempts"),
            ("activation_request_id", 1, "activation request ID"),
        )
        for field, value, message in mismatches:
            with self.subTest(field=field):
                mismatch = replace(
                    snapshot,
                    status=replace(snapshot.status, **{field: value}),
                )
                with self.assertRaisesRegex(C906LBringupError, message):
                    validate_c906l_snapshot(mismatch)

    def test_stale_heartbeat_is_rejected(self):
        snapshot = valid_snapshot(heartbeat=10)
        with self.assertRaisesRegex(C906LBringupError, "did not advance"):
            validate_c906l_snapshot_pair(snapshot, snapshot)


class LayoutTests(unittest.TestCase):
    def test_known_safe_layout(self):
        validate_c906l_layout(
            firmware_size=0x3000,
            run_address=0x88000000,
            shmem_address=0x88100000,
            scratch_address=0x82000000,
            scratch_size=0x100000,
        )

    def test_rejects_scratch_smaller_than_cache_eviction_minimum(self):
        with self.assertRaisesRegex(C906LBringupError, "at least"):
            validate_c906l_layout(
                0x3000, 0x88000000, 0x88100000,
                0x82000000, 0x40000,
            )

    def test_rejects_overlapping_ranges(self):
        with self.assertRaisesRegex(C906LBringupError, "overlap"):
            validate_c906l_layout(
                0x200000, 0x88000000, 0x88100000,
                0x82000000, 0x100000,
            )

    def test_rejects_mmio_as_scratch(self):
        with self.assertRaisesRegex(C906LBringupError, "outside SG2002 DRAM"):
            validate_c906l_layout(
                0x3000, 0x88000000, 0x88100000,
                0x03000000, 0x100000,
            )


class ResetSequenceTests(unittest.TestCase):
    def test_handoff_mode_accepts_held_reset(self):
        self.assertEqual(
            select_c906l_handoff_mode(0, False, False), "held-reset"
        )

    def test_handoff_mode_accepts_fsbl_release_from_this_transfer(self):
        self.assertEqual(
            select_c906l_handoff_mode(C906L_RESET_BIT, True, False),
            "fsbl-started",
        )

    def test_handoff_mode_rejects_ambiguous_running_core(self):
        with self.assertRaisesRegex(C906LBringupError, "ambiguous attach"):
            select_c906l_handoff_mode(C906L_RESET_BIT, False, False)

    def test_handoff_mode_allows_explicit_running_attach(self):
        self.assertEqual(
            select_c906l_handoff_mode(C906L_RESET_BIT, False, True),
            "explicit-attach",
        )

    def test_vendor_order_preserves_unrelated_register_bits(self):
        registers = {
            C906L_RESET_REG: 0xA5,
            C906L_SEC_SYS_REG: 0x100,
            C906L_VECTOR_LOW_REG: 0,
            C906L_VECTOR_HIGH_REG: 0xFFFFFFFF,
        }
        operations = []

        def read_u32(address):
            operations.append(("read", address))
            return registers[address]

        def write_u32(address, value):
            operations.append(("write", address, value))
            registers[address] = value

        apply_c906l_reset_sequence(0x88000000, read_u32, write_u32)

        self.assertEqual(registers[C906L_RESET_REG], 0xA5 | C906L_RESET_BIT)
        self.assertEqual(
            registers[C906L_SEC_SYS_REG], 0x100 | C906L_SEC_ENABLE_BIT
        )
        self.assertEqual(registers[C906L_VECTOR_LOW_REG], 0x88000000)
        self.assertEqual(registers[C906L_VECTOR_HIGH_REG], 0)
        self.assertEqual(operations, [
            ("read", C906L_RESET_REG),
            ("write", C906L_RESET_REG, 0xA5 & ~C906L_RESET_BIT),
            ("read", C906L_RESET_REG),
            ("read", C906L_SEC_SYS_REG),
            ("write", C906L_SEC_SYS_REG, 0x100 | C906L_SEC_ENABLE_BIT),
            ("read", C906L_SEC_SYS_REG),
            ("write", C906L_VECTOR_LOW_REG, 0x88000000),
            ("write", C906L_VECTOR_HIGH_REG, 0),
            ("read", C906L_VECTOR_LOW_REG),
            ("read", C906L_VECTOR_HIGH_REG),
            ("read", C906L_RESET_REG),
            ("write", C906L_RESET_REG, 0xA5 | C906L_RESET_BIT),
            ("read", C906L_RESET_REG),
        ])

    def test_failed_reset_assertion_never_programs_or_releases_core(self):
        operations = []

        def read_u32(address):
            operations.append(("read", address))
            # Model hardware refusing to clear the release bit.
            return C906L_RESET_BIT

        def write_u32(address, value):
            operations.append(("write", address, value))

        with self.assertRaisesRegex(C906LBringupError, "did not assert"):
            apply_c906l_reset_sequence(0x88000000, read_u32, write_u32)
        self.assertEqual(operations, [
            ("read", C906L_RESET_REG),
            ("write", C906L_RESET_REG, 0),
            ("read", C906L_RESET_REG),
        ])

if __name__ == "__main__":
    unittest.main()
