#!/bin/bash
# 使用 auditwheel 生成自包含的 wheel（silolink fork workflow）
# 打包所有依赖库，生成可在 Linux 上直接 pip install 的 wheel

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "=== Building Self-Contained Wheel with auditwheel ==="
echo ""

# 检查编译产物
if [ ! -f "$REPO_ROOT/build_fast/ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so" ]; then
    echo "ERROR: Compiled .so file not found!"
    echo "Please run ./compile_ifcopenshell.sh first"
    exit 1
fi

# 检查 Docker 镜像
if ! docker image inspect ifcopenshell-build >/dev/null 2>&1; then
    echo "ERROR: Docker image 'ifcopenshell-build' not found!"
    exit 1
fi

# 创建输出目录
mkdir -p wheels
mkdir -p wheel_temp

echo "Step 1: Building Python wheel in Docker..."
echo "Step 2: Using auditwheel to bundle dependencies into the wheel..."
echo ""

# 我们在 Ubuntu 22.04 (glibc 2.35) 上编译，所以能稳定产出 manylinux_2_35 wheel。
# 如果你需要更老 glibc（如 manylinux_2_31），必须在更老的 toolchain / 基础镜像上编译。
TARGET_PLAT="manylinux_2_35_x86_64"
PY_TAG="py310"
ABI_TAG="none"

# 在 Docker 中构建 wheel
docker run --rm \
  -v "$REPO_ROOT:/workspace" \
  ifcopenshell-build sh -c "
    set -e
    
    echo 'Installing build tooling...'
    pip3 install -q build wheel auditwheel
    
    echo 'Preparing Python package for wheel build...'
    cd /workspace
    rm -rf /workspace/wheel_build /workspace/wheel_temp
    mkdir -p /workspace/wheel_build

    # 复制 ifcopenshell-python 项目到 wheel_build（避免污染源码目录）
    cp -r /workspace/src/ifcopenshell-python/* /workspace/wheel_build/

    # 将编译好的二进制注入到 wheel_build/ifcopenshell 包内
    cp /workspace/build_fast/ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so \
       /workspace/wheel_build/ifcopenshell/
    cp /workspace/build_fast/ifcwrap/ifcopenshell_wrapper.py \
       /workspace/wheel_build/ifcopenshell/ 2>/dev/null || true

    echo 'Building wheel (python -m build)...'
    cd /workspace/wheel_build
    python3 -m build --wheel

    echo 'Repairing wheel with auditwheel (bundle external .so deps)...'
    mkdir -p /workspace/wheels
    mkdir -p /workspace/wheel_temp

    # python -m build 产出的 wheel 通常是 py3-none-any（因为项目是纯 Python 打包 + 内置 .so 文件）。
    # 但我们的二进制实际只适配 Python 3.10，所以先把文件名重命名为 py310-none-<plat>，方便 auditwheel 正确识别架构/平台。
    WHL=\$(ls dist/*.whl | head -1)
    if [ -z \"\$WHL\" ]; then
        echo 'ERROR: No wheel produced in dist/'
        exit 1
    fi

    NEW_WHL=\"dist/ifcopenshell-0.8.4+modified-${PY_TAG}-${ABI_TAG}-${TARGET_PLAT}.whl\"
    mv \"\$WHL\" \"\$NEW_WHL\"

    auditwheel repair \"\$NEW_WHL\" -w /workspace/wheels --plat ${TARGET_PLAT}
    
    echo ''
    echo '=== Wheel build complete! ==='
    ls -lh /workspace/wheels/*.whl 2>/dev/null || echo 'No wheel found'
  "

echo ""
echo "=== Wheel build complete! ==="
if ls wheels/*.whl 1> /dev/null 2>&1; then
    echo "Generated wheel(s):"
    ls -lh wheels/*.whl
    echo ""
    echo "You can now install it with:"
    echo "  pip install wheels/ifcopenshell-*.whl"
else
    echo "ERROR: No wheel file generated!"
    echo "Check the output above for errors."
    exit 1
fi

