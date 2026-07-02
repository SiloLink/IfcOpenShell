#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$REPO_ROOT"

tmp_output="$(mktemp)"
trap 'rm -f "$tmp_output"' EXIT

DRY_RUN=1 IMAGE=ifcopenshell-build-py311-amd64-test BUILD_JOBS=7 \
  ./silolink/scripts/build_py311_amd64_wheel.sh >"$tmp_output"

grep -q "nix/build-all.py" "$tmp_output"
grep -q "IfcOpenShell-Python" "$tmp_output"
grep -q -- "-py-311" "$tmp_output"
grep -q "rockylinux9-x64" "$tmp_output"
grep -q "OCCT 7.8.1" "$tmp_output"
grep -q "ADD_COMMIT_SHA=" "$tmp_output"
grep -q "Wheel ABI tag:       cp311" "$tmp_output"
grep -q "ifcopenshell-0.8.5+silolink.1-cp311-cp311-manylinux_2_31_x86_64.whl" "$tmp_output"

grep -q "FROM rockylinux:9" silolink/docker/Dockerfile.py311-build
grep -q "Relocated upstream cache paths" silolink/scripts/build_py311_amd64_wheel.sh
grep -q "/__w/IfcOpenShell/IfcOpenShell/build" silolink/scripts/build_py311_amd64_wheel.sh
grep -q "Upstream swig cache is incomplete or not runnable" silolink/scripts/build_py311_amd64_wheel.sh
grep -q "Root-Is-Purelib: false" silolink/scripts/build_py311_amd64_wheel.sh
if grep -q 'assert "Tag: py3-none-any" in wheel_metadata' silolink/scripts/build_py311_amd64_wheel.sh; then
  echo "py311 wheel metadata must not be positively asserted as py3-none-any" >&2
  exit 1
fi
if grep -q "libocct-" silolink/docker/Dockerfile.py311-build; then
  echo "Dockerfile.py311-build must use upstream build-all OCCT, not distro libocct packages" >&2
  exit 1
fi
