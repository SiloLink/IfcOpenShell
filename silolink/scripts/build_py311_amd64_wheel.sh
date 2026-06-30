#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE="${IMAGE:-ifcopenshell-build-py311-amd64}"
BUILD_JOBS="${BUILD_JOBS:-2}"
BUILD_DIR="${BUILD_DIR:-build_py311_amd64}"
WHEEL_BUILD_DIR="${WHEEL_BUILD_DIR:-wheel_build_py311}"
WHEEL_TEMP_DIR="${WHEEL_TEMP_DIR:-wheel_temp_py311}"
PACKAGE_VERSION="${PACKAGE_VERSION:-0.8.5+silolink.1}"
TARGET_PLAT="${TARGET_PLAT:-manylinux_2_36_x86_64}"

cd "$REPO_ROOT"

docker image inspect "$IMAGE" >/dev/null

docker run --rm --platform linux/amd64 \
  -e BUILD_JOBS="$BUILD_JOBS" \
  -e BUILD_DIR="$BUILD_DIR" \
  -e WHEEL_BUILD_DIR="$WHEEL_BUILD_DIR" \
  -e WHEEL_TEMP_DIR="$WHEEL_TEMP_DIR" \
  -e PACKAGE_VERSION="$PACKAGE_VERSION" \
  -e TARGET_PLAT="$TARGET_PLAT" \
  -v "$REPO_ROOT:/workspace" \
  -w /workspace \
  "$IMAGE" bash -lc '
    set -euo pipefail

    python3 - <<PY
import platform, sys, sysconfig
print(sys.version)
print(platform.machine())
print(sysconfig.get_config_var("EXT_SUFFIX"))
PY

    rm -rf "$WHEEL_BUILD_DIR" "$WHEEL_TEMP_DIR"
    mkdir -p "$BUILD_DIR"

    cd "$BUILD_DIR"
    cmake ../cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCOLLADA_SUPPORT=OFF \
      -DGLTF_SUPPORT=ON \
      -DIFCXML_SUPPORT=OFF \
      -DPYTHON_EXECUTABLE=/usr/local/bin/python3 \
      -DPYTHON_INCLUDE_DIR=/usr/local/include/python3.11 \
      -DPython_EXECUTABLE=/usr/local/bin/python3 \
      -DPython_INCLUDE_DIR=/usr/local/include/python3.11 \
      -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/opencascade \
      -DOCC_INCLUDE_DIR=/usr/include/opencascade \
      -DOCC_LIBRARY_DIR=/usr/lib/x86_64-linux-gnu \
      -DOpenCASCADE_DIR=/usr/lib/x86_64-linux-gnu/cmake/opencascade

    make -j "$BUILD_JOBS" ifcopenshell_wrapper

    cd /workspace
    so_path="$(find "$BUILD_DIR/ifcwrap" -maxdepth 1 -name "_ifcopenshell_wrapper*.so" -print -quit)"
    if [ -z "$so_path" ]; then
      echo "ERROR: compiled IfcOpenShell wrapper was not produced"
      exit 1
    fi
    case "$so_path" in
      *"_ifcopenshell_wrapper.cpython-311-x86_64-linux-gnu.so") ;;
      *)
        echo "ERROR: unexpected wrapper ABI: $so_path"
        exit 1
        ;;
    esac

    mkdir -p "$WHEEL_BUILD_DIR" "$WHEEL_TEMP_DIR" wheels
    cp -a src/ifcopenshell-python/. "$WHEEL_BUILD_DIR/"
    python3 - <<PY
from pathlib import Path
path = Path("$WHEEL_BUILD_DIR/pyproject.toml")
text = path.read_text()
old = "version = \\"0.0.0\\""
new = "version = \\"$PACKAGE_VERSION\\""
if old not in text:
    raise SystemExit(f"expected {old!r} in {path}")
path.write_text(text.replace(old, new))
PY

    cp "$so_path" "$WHEEL_BUILD_DIR/ifcopenshell/"
    cp "$BUILD_DIR/ifcwrap/ifcopenshell_wrapper.py" "$WHEEL_BUILD_DIR/ifcopenshell/"

    cd "$WHEEL_BUILD_DIR"
    python3 -m build --wheel
    built_wheel="$(ls dist/*.whl | head -n 1)"
    if [ -z "$built_wheel" ]; then
      echo "ERROR: wheel build produced no artifact"
      exit 1
    fi

    python3 -m wheel tags \
      --remove \
      --python-tag cp311 \
      --abi-tag cp311 \
      --platform-tag linux_x86_64 \
      "$built_wheel"
    retagged_wheel="$(find . -maxdepth 2 -name "ifcopenshell-${PACKAGE_VERSION}-cp311-cp311-linux_x86_64.whl" -print -quit)"
    if [ -z "$retagged_wheel" ]; then
      echo "ERROR: wheel retag produced no artifact"
      exit 1
    fi

    auditwheel repair "$retagged_wheel" -w /workspace/wheels --plat "$TARGET_PLAT"

    repaired_wheel="$(ls /workspace/wheels/ifcopenshell-${PACKAGE_VERSION}-cp311-cp311-manylinux*_x86_64*.whl | head -n 1)"
    auditwheel show "$repaired_wheel"
    python3 - <<PY
import zipfile
from pathlib import Path

wheel = Path("$repaired_wheel")
with zipfile.ZipFile(wheel) as zf:
    metadata_name = next(name for name in zf.namelist() if name.endswith(".dist-info/METADATA"))
    wheel_name = next(name for name in zf.namelist() if name.endswith(".dist-info/WHEEL"))
    metadata = zf.read(metadata_name).decode()
    wheel_metadata = zf.read(wheel_name).decode()
    assert "Version: $PACKAGE_VERSION" in metadata, metadata
    assert "Tag: cp311-cp311-" in wheel_metadata, wheel_metadata
    assert "Tag: py3-none-any" not in wheel_metadata, wheel_metadata
    assert "$TARGET_PLAT" in wheel_metadata, wheel_metadata
    assert any(name.endswith("_ifcopenshell_wrapper.cpython-311-x86_64-linux-gnu.so") for name in zf.namelist())
print(wheel)
PY
  '
