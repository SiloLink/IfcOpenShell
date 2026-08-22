#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

IMAGE="${IMAGE:-ifcopenshell-build-py311-amd64}"
BUILD_JOBS="${BUILD_JOBS:-8}"
BUILD_DIR="${BUILD_DIR:-build_py311_official}"
WHEEL_BUILD_DIR="${WHEEL_BUILD_DIR:-wheel_build_py311}"
WHEEL_TEMP_DIR="${WHEEL_TEMP_DIR:-wheel_temp_py311}"
PACKAGE_VERSION="${PACKAGE_VERSION:-0.9.0+silolink.1}"
PYTHON_VERSION="${PYTHON_VERSION:-3.11.8}"
PYTHON_TAG="${PYTHON_TAG:-cp311}"
ABI_TAG="${ABI_TAG:-cp311}"
PYTHON_BUILD_FLAG="${PYTHON_BUILD_FLAG:--py-311}"
TARGET_PLAT="${TARGET_PLAT:-manylinux_2_31_x86_64}"
USE_UPSTREAM_CACHE="${USE_UPSTREAM_CACHE:-1}"
UPSTREAM_CACHE_REPO="${UPSTREAM_CACHE_REPO:-https://github.com/IfcOpenShell/build-outputs.git}"
UPSTREAM_CACHE_REF="${UPSTREAM_CACHE_REF:-rockylinux9-x64}"
ADD_COMMIT_SHA="${ADD_COMMIT_SHA:-}"
DRY_RUN="${DRY_RUN:-0}"

cd "$REPO_ROOT"

cat <<EOF
=== Building Silolink IfcOpenShell py311 wheel ===
Builder image:        $IMAGE
Build directory:      $BUILD_DIR
Python:              $PYTHON_VERSION ($PYTHON_TAG / $PYTHON_BUILD_FLAG)
Package version:      $PACKAGE_VERSION
Wheel ABI tag:       $ABI_TAG
Wheel platform tag:   $TARGET_PLAT
Dependency stack:     upstream nix/build-all.py, OCCT 7.8.1
Upstream cache:       $USE_UPSTREAM_CACHE ($UPSTREAM_CACHE_REF)
Add commit SHA:       $ADD_COMMIT_SHA
EOF

if [ "$DRY_RUN" = "1" ]; then
  cat <<EOF
DRY RUN:
  docker run --platform linux/amd64 -v "$REPO_ROOT:/workspace" "$IMAGE"
  cache branch: $UPSTREAM_CACHE_REF
  build command: BUILD_CFG=Release IFCOS_NUM_BUILD_PROCS=$BUILD_JOBS ADD_COMMIT_SHA=$ADD_COMMIT_SHA python3.11 ./nix/build-all.py -v --diskcleanup $PYTHON_BUILD_FLAG IfcOpenShell-Python
  package command: python3.11 -m build --wheel, repacked to ifcopenshell-$PACKAGE_VERSION-$PYTHON_TAG-$ABI_TAG-$TARGET_PLAT.whl
EOF
  exit 0
fi

docker image inspect "$IMAGE" >/dev/null

docker run --rm --platform linux/amd64 \
  -e BUILD_JOBS="$BUILD_JOBS" \
  -e BUILD_DIR="$BUILD_DIR" \
  -e WHEEL_BUILD_DIR="$WHEEL_BUILD_DIR" \
  -e WHEEL_TEMP_DIR="$WHEEL_TEMP_DIR" \
  -e PACKAGE_VERSION="$PACKAGE_VERSION" \
  -e PYTHON_VERSION="$PYTHON_VERSION" \
  -e PYTHON_TAG="$PYTHON_TAG" \
  -e ABI_TAG="$ABI_TAG" \
  -e PYTHON_BUILD_FLAG="$PYTHON_BUILD_FLAG" \
  -e TARGET_PLAT="$TARGET_PLAT" \
  -e USE_UPSTREAM_CACHE="$USE_UPSTREAM_CACHE" \
  -e UPSTREAM_CACHE_REPO="$UPSTREAM_CACHE_REPO" \
  -e UPSTREAM_CACHE_REF="$UPSTREAM_CACHE_REF" \
  -e ADD_COMMIT_SHA="$ADD_COMMIT_SHA" \
  -v "$REPO_ROOT:/workspace:Z" \
  -w /workspace \
  "$IMAGE" bash -lc '
    set -euo pipefail

    python3.11 - <<PY
import platform, re, sys
from pathlib import Path

print(sys.version)
print(platform.machine())
text = Path("nix/build-all.py").read_text()
match = re.search(r"^OCCT_VERSION = \"([^\"]+)\"", text, re.MULTILINE)
if not match:
    raise SystemExit("Could not find OCCT_VERSION in nix/build-all.py")
if match.group(1) != "7.8.1":
    raise SystemExit(f"Expected upstream OCCT 7.8.1, got {match.group(1)}")
print(f"OCCT_VERSION={match.group(1)}")
PY

    resolve_workspace_child() {
      python3.11 - "$1" <<PY
import sys
from pathlib import Path

root = Path("/workspace").resolve()
configured = Path(sys.argv[1])
candidate = configured.resolve() if configured.is_absolute() else (root / configured).resolve()
try:
    candidate.relative_to(root)
except ValueError:
    raise SystemExit(f"Refusing path outside {root}: {candidate}")
if candidate == root:
    raise SystemExit(f"Refusing workspace root as a build path: {candidate}")
print(candidate)
PY
    }

    BUILD_DIR="$(resolve_workspace_child "$BUILD_DIR")"
    WHEEL_BUILD_DIR="$(resolve_workspace_child "$WHEEL_BUILD_DIR")"
    WHEEL_TEMP_DIR="$(resolve_workspace_child "$WHEEL_TEMP_DIR")"
    export BUILD_DIR WHEEL_BUILD_DIR WHEEL_TEMP_DIR
    export BUILD_CFG=Release
    export IFCOS_NUM_BUILD_PROCS="$BUILD_JOBS"
    export USE_OCCT=true
    export ADD_COMMIT_SHA="$ADD_COMMIT_SHA"

    if [ "$USE_UPSTREAM_CACHE" = "1" ]; then
      if [ ! -d "$BUILD_DIR/.git" ]; then
        rm -rf "$BUILD_DIR"
        echo "Cloning upstream build cache $UPSTREAM_CACHE_REF..."
        git clone --depth 1 --branch "$UPSTREAM_CACHE_REF" "$UPSTREAM_CACHE_REPO" "$BUILD_DIR"
        (cd "$BUILD_DIR" && git lfs pull)
      else
        echo "Refreshing existing upstream build cache..."
        (cd "$BUILD_DIR" && git fetch --depth 1 origin "$UPSTREAM_CACHE_REF" && git checkout FETCH_HEAD && git lfs pull)
      fi

      if ! find "$BUILD_DIR" -name "cache-*.tar.gz" -print -quit | grep -q .; then
        echo "ERROR: upstream dependency cache contains no cache archives" >&2
        exit 1
      fi

      (cd "$BUILD_DIR" && python3.11 /workspace/nix/cache_dependencies.py unpack)
      python3.11 - <<PY
from pathlib import Path

old_roots = [
    "/__w/IfcOpenShell/IfcOpenShell/build/Linux/x86_64",
    "/__w/IfcOpenShell/IfcOpenShell/build",
]
new_roots = [
    "$BUILD_DIR/Linux/x86_64",
    "$BUILD_DIR",
]
search_roots = [
    Path("$BUILD_DIR/Linux/x86_64/install"),
    Path("$BUILD_DIR/Linux/x86_64/build"),
]

rewritten = 0
for root in search_roots:
    if not root.exists():
        continue
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        try:
            text = path.read_text()
        except UnicodeDecodeError:
            continue
        updated = text
        for old, new in zip(old_roots, new_roots):
            updated = updated.replace(old, new)
        if updated != text:
            path.write_text(updated)
            rewritten += 1

print(f"Relocated upstream cache paths in {rewritten} text files")
PY
      swig_executable="$BUILD_DIR/Linux/x86_64/install/swig-4.2.1/bin/swig"
      swig_lib_dir=""
      if [ -x "$swig_executable" ]; then
        swig_lib_dir="$("$swig_executable" -swiglib 2>/dev/null || true)"
      fi
      if [ ! -x "$swig_executable" ] || [ ! -d "$swig_lib_dir" ]; then
        echo "Upstream swig cache is incomplete or not runnable; removing it so build-all.py rebuilds swig."
        rm -rf "$BUILD_DIR/Linux/x86_64/install/swig-4.2.1" \
               "$BUILD_DIR/Linux/x86_64/build/swig-4.2.1"
      fi
    else
      mkdir -p "$BUILD_DIR"
    fi

    echo "Running upstream build-all.py for py311 IfcOpenShell-Python..."
    python3.11 ./nix/build-all.py -v --diskcleanup "$PYTHON_BUILD_FLAG" IfcOpenShell-Python

    deps_dir="$BUILD_DIR/Linux/x86_64"
    module_root="$deps_dir/install/ifcopenshell/python-$PYTHON_VERSION"
    if [ ! -d "$module_root" ]; then
      echo "ERROR: expected ifcopenshell module output at $module_root"
      exit 1
    fi
    module_pkg="$module_root"

    wrapper_so="$(find "$module_pkg" -maxdepth 1 -name "_ifcopenshell_wrapper.cpython-311-x86_64-linux-gnu.so" -print -quit)"
    if [ -z "$wrapper_so" ]; then
      echo "ERROR: py311 wrapper was not produced in $module_pkg"
      find "$module_root" -name "*ifcopenshell_wrapper*" -print
      exit 1
    fi

    echo "Built wrapper:"
    ls -lh "$wrapper_so"
    ldd "$wrapper_so" | tee /tmp/ifcopenshell-wrapper-ldd.txt
    if grep -q "libTK" /tmp/ifcopenshell-wrapper-ldd.txt; then
      echo "ERROR: wrapper dynamically links OpenCascade libTK*.so; expected upstream static OCCT layout"
      exit 1
    fi

    rm -rf "$WHEEL_BUILD_DIR" "$WHEEL_TEMP_DIR"
    mkdir -p "$WHEEL_BUILD_DIR" "$WHEEL_TEMP_DIR" wheels
    cp -a src/ifcopenshell-python/. "$WHEEL_BUILD_DIR/"

    python3.11 - <<PY
from pathlib import Path

package_version = "$PACKAGE_VERSION"
for rel in ("pyproject.toml", "ifcopenshell/__init__.py"):
    path = Path("$WHEEL_BUILD_DIR") / rel
    text = path.read_text()
    old = "version = \"0.0.0\""
    if old not in text:
        raise SystemExit(f"expected {old!r} in {path}")
    path.write_text(text.replace(old, f"version = \"{package_version}\""))
PY

    native_modules=("$module_pkg"/*.so)
    if [ "${#native_modules[@]}" -lt 2 ]; then
      echo "ERROR: v0.9 native plugin modules were not produced in $module_pkg"
      exit 1
    fi
    cp "${native_modules[@]}" "$WHEEL_BUILD_DIR/ifcopenshell/"
    wrapper_py="$module_pkg/ifcopenshell_wrapper.py"
    if [ ! -f "$wrapper_py" ]; then
      echo "ERROR: py311 wrapper module was not produced at $wrapper_py"
      exit 1
    fi
    cp "$wrapper_py" "$WHEEL_BUILD_DIR/ifcopenshell/"

    cd "$WHEEL_BUILD_DIR"
    python3.11 -m build --wheel
    built_wheel="$(ls dist/*.whl | head -n 1)"
    if [ -z "$built_wheel" ]; then
      echo "ERROR: wheel build produced no artifact"
      exit 1
    fi

    final_wheel="/workspace/wheels/ifcopenshell-${PACKAGE_VERSION}-${PYTHON_TAG}-${ABI_TAG}-${TARGET_PLAT}.whl"
    rm -f "$final_wheel"

    python3.11 - <<PY
import base64
import csv
import hashlib
import io
import os
import tempfile
import zipfile
from pathlib import Path

source_wheel = Path("$built_wheel")
wheel = Path("$final_wheel")
python_tag = "$PYTHON_TAG"
abi_tag = "$ABI_TAG"
platform_tag = "$TARGET_PLAT"
expected_tag = f"{python_tag}-{abi_tag}-{platform_tag}"

def record_digest(data):
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return f"sha256={digest}", str(len(data))

with zipfile.ZipFile(source_wheel) as zf:
    names = zf.namelist()
    metadata_name = next(name for name in names if name.endswith(".dist-info/METADATA"))
    wheel_name = next(name for name in names if name.endswith(".dist-info/WHEEL"))
    record_name = next(name for name in names if name.endswith(".dist-info/RECORD"))
    metadata = zf.read(metadata_name).decode()
    wheel_metadata = zf.read(wheel_name).decode()
    assert "Version: $PACKAGE_VERSION" in metadata, metadata
    assert any(name.endswith("_ifcopenshell_wrapper.cpython-311-x86_64-linux-gnu.so") for name in names)
    assert any(name.endswith("ifcopenshell_document_rdb.so") for name in names)
    assert not any(".libs/" in name for name in names), "unexpected bundled shared library directory"

    updated_wheel_lines = []
    saw_root = False
    for line in wheel_metadata.splitlines():
        if not line:
            continue
        if line.startswith("Root-Is-Purelib:"):
            updated_wheel_lines.append("Root-Is-Purelib: false")
            saw_root = True
        elif not line.startswith("Tag:"):
            updated_wheel_lines.append(line)
    if not saw_root:
        updated_wheel_lines.append("Root-Is-Purelib: false")
    updated_wheel_lines.append(f"Tag: {expected_tag}")
    updated_wheel_metadata = "\n".join(updated_wheel_lines).rstrip() + "\n\n"

    entries = []
    for info in zf.infolist():
        if info.filename == record_name:
            continue
        data = zf.read(info.filename)
        if info.filename == wheel_name:
            data = updated_wheel_metadata.encode()
        entries.append((info, data))

with tempfile.NamedTemporaryFile(delete=False, dir=wheel.parent, suffix=".whl") as tmp:
    tmp_path = Path(tmp.name)

try:
    with zipfile.ZipFile(tmp_path, "w", compression=zipfile.ZIP_DEFLATED) as out:
        record_rows = []
        for info, data in entries:
            out.writestr(info, data)
            digest, size = record_digest(data)
            record_rows.append([info.filename, digest, size])
        record_rows.append([record_name, "", ""])

        record_buffer = io.StringIO()
        writer = csv.writer(record_buffer, lineterminator="\n")
        writer.writerows(record_rows)
        out.writestr(record_name, record_buffer.getvalue().encode())
    os.replace(tmp_path, wheel)
finally:
    if tmp_path.exists():
        tmp_path.unlink()

with zipfile.ZipFile(wheel) as zf:
    names = zf.namelist()
    wheel_name = next(name for name in names if name.endswith(".dist-info/WHEEL"))
    record_name = next(name for name in names if name.endswith(".dist-info/RECORD"))
    wheel_metadata = zf.read(wheel_name).decode()
    record_text = zf.read(record_name).decode()
    assert "Root-Is-Purelib: false" in wheel_metadata, wheel_metadata
    assert f"Tag: {expected_tag}" in wheel_metadata, wheel_metadata
    assert "Tag: py3-none-any" not in wheel_metadata, wheel_metadata
    assert "\n\nTag:" not in wheel_metadata, wheel_metadata
    wheel_data = zf.read(wheel_name)
    digest, size = record_digest(wheel_data)
    assert f"{wheel_name},{digest},{size}" in record_text, record_text
os.chmod(wheel, 0o644)
print(wheel)
PY

    verify_python="$deps_dir/install/python-$PYTHON_VERSION/bin/python3"
    if [ ! -x "$verify_python" ]; then
      echo "ERROR: built Python $PYTHON_VERSION executable not found at $verify_python"
      exit 1
    fi
    "$verify_python" -m venv "$WHEEL_TEMP_DIR/verify-venv"
    "$WHEEL_TEMP_DIR/verify-venv/bin/python" -m pip install --upgrade pip >/dev/null
    "$WHEEL_TEMP_DIR/verify-venv/bin/python" -m pip install "$final_wheel" numpy >/dev/null
    "$WHEEL_TEMP_DIR/verify-venv/bin/python" - <<PY
import ifcopenshell
import ifcopenshell.geom
print("verify import", ifcopenshell.version)
PY

    ls -lh "$final_wheel"
  '
