#!/usr/bin/env python3
"""Execute the production power transport against a simulated firmware peer."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def declaration(source, pattern):
    match = re.search(pattern, source, re.MULTILINE)
    if match is None:
        raise AssertionError(pattern)
    brace = source.index("{", match.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    return source[match.start():end]


source = Path(sys.argv[1]).read_text()
production = declaration(source, r"^struct power_record\s*") + "\n"
for name in ("generation_valid", "request_address", "acknowledged", "set_power_locked",
             "power_enable", "power_disable", "power_is_enabled"):
    production += declaration(source, rf"^static [^;\n]*\b{name}\([^;{{]*\)") + "\n"
harness = (Path(sys.argv[3]) if len(sys.argv) > 3 else
           Path(__file__).with_name("test_transport.c")).read_text()
with tempfile.TemporaryDirectory(prefix="c906l-power-test-") as directory:
    root = Path(directory)
    (root / "test.c").write_text(harness.replace("/* @PRODUCTION@ */", production))
    subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-I", sys.argv[2],
                    str(root / "test.c"), "-o", str(root / "test")], check=True)
    subprocess.run([str(root / "test")], check=True, timeout=10)
print("Production Wi-Fi power transport tests passed")
