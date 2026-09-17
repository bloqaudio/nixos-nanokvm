#!/usr/bin/env bash
set -euo pipefail

# Explicit x86_64 build-host targets; the SD images cross-compile for RISC-V.
# Keep this independent of developer keys and Wi-Fi configuration.
nix build --no-update-lock-file --print-build-logs --json --out-link result-ci \
  .#packages.x86_64-linux.nanokvm-server \
  .#legacyPackages.x86_64-linux.boards.pcie.mainline.sd \
  .#legacyPackages.x86_64-linux.boards.picoclaw.mainline.sd.c906l-lcd \
  .#checks.x86_64-linux.sg2002-h264-bridge-colour \
  .#checks.x86_64-linux.sg2002-vpss-state \
  .#checks.x86_64-linux.sg2002-c906l-module-eval \
  .#checks.x86_64-linux.sg2002-c906l-picoclaw-module-eval \
  .#checks.x86_64-linux.sg2002-c906l-picoclaw-sd-module-eval \
  .#checks.x86_64-linux.sg2002-c906l-picoclaw-dtb \
  .#checks.x86_64-linux.sg2002-c906l-picoclaw-control \
  .#checks.x86_64-linux.sg2002-c906l-picoclaw-framebuffer \
  .#checks.x86_64-linux.sg2002-c906l-contract-generator \
  .#checks.x86_64-linux.sg2002-c906l-rust \
  > ci-build-results.json
