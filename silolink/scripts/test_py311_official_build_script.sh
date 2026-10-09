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
grep -q -- "--occt-shared" "$tmp_output"
grep -q -- '--schemas "2x3;4;4x1;4x2;4x3;4x3_tc1;4x3_add1;4x3_add2"' "$tmp_output"
grep -q "rockylinux9-x64" "$tmp_output"
grep -q "OCCT 7.8.1" "$tmp_output"
grep -q "ADD_COMMIT_SHA=" "$tmp_output"
grep -q "Wheel ABI tag:       cp311" "$tmp_output"
grep -q "ifcopenshell-0.9.1+silolink.1-cp311-cp311-manylinux_2_34_x86_64.whl" "$tmp_output"

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

# Reusing dependencies must remove obsolete plug-ins without deleting Python or
# unrelated installed packages.
python3 - <<'PY'
from pathlib import Path
import re
import tempfile

script = Path("silolink/scripts/build_py311_amd64_wheel.sh").read_text()
cleanup = re.search(r"python3\.11 - <<PY\n(import shutil\n.*?)\nPY", script, re.DOTALL)
assert cleanup, "missing cached-package cleanup"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    prefix = root / "Linux/x86_64/install/python-3.11.8"
    package = prefix / "lib/python3.11/site-packages/ifcopenshell"
    package.mkdir(parents=True)
    (package / "obsolete-abi-v1.so").write_text("stale")
    dependency = package.parent / "numpy"
    dependency.mkdir()
    (dependency / "__init__.py").write_text("keep")
    python = prefix / "bin/python3"
    python.parent.mkdir()
    python.write_text("keep")
    other_python = root / "Linux/x86_64/install/python-3.12.1/lib/python3.12/site-packages/ifcopenshell"
    other_python.mkdir(parents=True)
    code = cleanup[1].replace("$BUILD_DIR", str(root)).replace("$PYTHON_VERSION", "3.11.8")
    exec(compile(code, "cached-package-cleanup", "exec"), {})
    assert not package.exists()
    assert (dependency / "__init__.py").read_text() == "keep"
    assert python.read_text() == "keep"
    assert other_python.is_dir()
    exec(compile(code, "cached-package-cleanup", "exec"), {})
PY
