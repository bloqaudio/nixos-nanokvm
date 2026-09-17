#!/usr/bin/env bash
set -euo pipefail

manifest_dir=/nix/.rw-store/nanokvm-cache
run="${GITHUB_RUN_ID:?}-${GITHUB_RUN_ATTEMPT:?}"
revision="${GITHUB_SHA:?}"
umask 022
temporary=$(mktemp "$manifest_dir/.outputs.XXXXXX")
trap 'rm -f "$temporary"' EXIT
jq --arg revision "$revision" --arg run "$run" \
  '{revision: $revision, run: $run, paths: ([.[].outputs[]] | unique)}' \
  ci-build-results.json > "$temporary"
chmod 644 "$temporary"
mv "$temporary" "$manifest_dir/outputs.json"

# The host polls once per minute, imports full closures, and Harmonia signs
# them. Wait for public availability before allowing another run to publish.
deadline=$((SECONDS + 7200))
mapfile -t paths < <(jq -r '.paths[]' "$manifest_dir/outputs.json")
for path in "${paths[@]}"; do
  hash=${path#/nix/store/}
  hash=${hash%%-*}
  while ! curl --fail --silent --show-error --max-time 30 \
    "https://cache.hellas.ai/$hash.narinfo" -o /dev/null; do
    if (( SECONDS >= deadline )); then
      echo "Timed out waiting for cache publication: $path" >&2
      exit 1
    fi
    sleep 15
  done
done
printf 'Cached %s outputs for %s\n' "${#paths[@]}" "$revision"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  printf 'Built and cached **%s outputs** for %s at https://cache.hellas.ai.\n' \
    "${#paths[@]}" "$revision" >> "$GITHUB_STEP_SUMMARY"
fi
