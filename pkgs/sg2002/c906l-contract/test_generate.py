#!/usr/bin/env python3
"""Focused host tests for deterministic C906L contract generation."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path


def load_generator(path: Path):
    spec = importlib.util.spec_from_file_location("c906l_contract_generator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load generator from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ContractGenerationTests(unittest.TestCase):
    generator_path: Path
    profile_arguments: list[tuple[str, Path, str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.generator = load_generator(cls.generator_path)
        cls.profiles = {}
        for name, path, digest in cls.profile_arguments:
            contract, semantic = cls.generator.load_resolved(path, digest)
            if name in cls.profiles:
                raise RuntimeError(f"duplicate profile argument: {name}")
            cls.profiles[name] = (path, digest, contract, semantic)
        cls.base_path, cls.base_sha256, cls.base, cls.base_semantic = cls.profiles[
            "base"
        ]
        (
            cls.timer4_path,
            cls.timer4_sha256,
            cls.timer4,
            cls.timer4_semantic,
        ) = cls.profiles["timer4"]

    def generate(self, contract: Path, digest: str, output: Path) -> None:
        self.generator.generate(contract, digest, output)

    @staticmethod
    def tree_bytes(root: Path) -> dict[str, bytes]:
        return {
            str(path.relative_to(root)): path.read_bytes()
            for path in sorted(root.rglob("*"))
            if path.is_file()
        }

    def test_profiles_have_exact_current_capabilities(self) -> None:
        self.assertEqual(self.base["profile"]["name"], "base")
        self.assertEqual(self.base["profile"]["expectedCapabilities"], 0x0B)
        self.assertEqual(self.base["profile"]["peripherals"], [])
        self.assertEqual(self.timer4["profile"]["name"], "timer4")
        self.assertEqual(self.timer4["profile"]["expectedCapabilities"], 0x0F)
        self.assertEqual(self.timer4["profile"]["peripherals"], ["timer4"])
        self.assertNotEqual(self.base_sha256, self.timer4_sha256)

    def test_digest_is_over_semantic_canonical_json(self) -> None:
        for name, (_path, digest, _contract, semantic) in self.profiles.items():
            with self.subTest(profile=name):
                self.assertEqual(hashlib.sha256(semantic).hexdigest(), digest)
        documented = dict(self.base)
        documented["spdxLicense"] = "documentation-only change"
        self.assertEqual(
            self.generator.canonical_json(
                self.generator.strip_documentation(documented)
            ),
            self.base_semantic,
        )

    def test_shared_memory_is_exactly_partitioned(self) -> None:
        shared = self.base["memory"]["shared"]
        cursor = 0
        for region in sorted(shared["regions"].values(), key=lambda item: item["offset"]):
            self.assertEqual(region["offset"], cursor)
            self.assertEqual(region["offset"] % 4096, 0)
            self.assertEqual(region["size"] % 4096, 0)
            cursor += region["size"]
        self.assertEqual(cursor, shared["size"])
        self.assertEqual(shared["address"] + shared["size"], 0x90000000)

    def test_vring_directions_match_linux_rpmsg(self) -> None:
        vrings = self.base["rpmsg"]["vrings"]
        self.assertEqual(vrings["deviceToDriverRegion"], "rpmsgVring0")
        self.assertEqual(vrings["driverToDeviceRegion"], "rpmsgVring1")

    def test_wire_layout_fixtures_are_exact(self) -> None:
        message = struct.pack("<BBHI", 1, 3, 0x1234, 0x89ABCDEF)
        self.assertEqual(message, bytes.fromhex("01 03 34 12 ef cd ab 89"))
        self.assertEqual(len(message), self.base["abi"]["message"]["size"])

        status = struct.pack(
            "<IHHIIIIQQQQ8s",
            0x4D564B4E,
            1,
            0,
            64,
            2,
            7,
            0,
            11,
            0x0B,
            0x1122334455667788,
            0x8877665544332211,
            bytes(8),
        )
        self.assertEqual(len(status), self.base["abi"]["status"]["size"])
        for field in self.base["abi"]["status"]["fields"]:
            self.assertLessEqual(field["offset"] + field["width"], len(status))

        resource_snapshot_format = "<" + "I" * 11 + "B" * 4 + "I" * 10
        self.assertEqual(
            struct.calcsize(resource_snapshot_format),
            self.base["rpmsg"]["resourceTable"]["serializedSize"],
        )

    def test_generation_is_byte_deterministic(self) -> None:
        for name, (path, digest, _contract, _semantic) in self.profiles.items():
            with self.subTest(profile=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                first = root / "first"
                second = root / "second"
                self.generate(path, digest, first)
                self.generate(path, digest, second)
                self.assertEqual(self.tree_bytes(first), self.tree_bytes(second))

    def test_generated_bindings_carry_profile_and_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            self.generate(self.base_path, self.base_sha256, output)
            generated = self.tree_bytes(output)
            self.assertEqual(
                generated["share/sg2002-c906l/contract.json"],
                self.base_path.read_bytes(),
            )
            header = generated["include/sg2002-c906l-contract.h"].decode()
            self.assertIn("SG2002_C906L_EXPECTED_CAPABILITIES UINT64_C(0x000000000000000b)", header)
            self.assertIn("SG2002_C906L_RPMSG_PAYLOAD_BYTES 496U", header)
            self.assertNotIn("SG2002_C906L_HAVE_TIMER4", header)
            self.assertIn("SG2002_C906L_STATUS_SIZE 64U", header)
            self.assertIn("__attribute__((packed, aligned(8)))", header)
            self.assertIn("__attribute__((packed, aligned(64)))", header)
            self.assertIn(
                "SG2002_C906L_STATUS_REGION_SIZE UINT64_C(0x0000000000001000)",
                header,
            )
            rust = generated["rust/generated_contract.rs"].decode()
            self.assertIn('pub const PROFILE_NAME: &str = "base";', rust)
            self.assertIn("pub const RPMSG_PAYLOAD_BYTES: usize = 496;", rust)
            dts = generated["dts/sg2002-c906l-contract.dtsi"].decode()
            self.assertIn("mboxes = <&mailbox 0 2>;", dts)
            self.assertIn("/bits/ 64 <0x000000000000000b>", dts)
            self.assertIn(self.base_sha256[:16], dts.replace(" ", ""))

    def test_generated_constant_names_are_unique(self) -> None:
        for name, (path, digest, _contract, _semantic) in self.profiles.items():
            with self.subTest(profile=name), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "out"
                self.generate(path, digest, output)
                header = (output / "include/sg2002-c906l-contract.h").read_text()
                definitions = [
                    line.split()[1]
                    for line in header.splitlines()
                    if line.startswith("#define SG2002_C906L_")
                ]
                self.assertEqual(len(definitions), len(set(definitions)))
                rust = (output / "rust/generated_contract.rs").read_text()
                constants = [
                    line.split()[2].rstrip(":")
                    for line in rust.splitlines()
                    if line.startswith("pub const ")
                ]
                self.assertEqual(len(constants), len(set(constants)))

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "timer4"
            self.generate(self.timer4_path, self.timer4_sha256, output)
            header = (output / "include/sg2002-c906l-contract.h").read_text()
            self.assertIn("SG2002_C906L_HAVE_TIMER4 1", header)

    def test_wrong_digest_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "contract digest mismatch"):
            self.generator.load_resolved(self.base_path, "0" * 64)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument(
        "--profile",
        action="append",
        nargs=3,
        required=True,
        metavar=("NAME", "PATH", "SHA256"),
    )
    args = parser.parse_args()

    ContractGenerationTests.generator_path = args.generator
    ContractGenerationTests.profile_arguments = [
        (name, Path(path), digest) for name, path, digest in args.profile
    ]
    unittest.main(argv=[__file__])


if __name__ == "__main__":
    main()
