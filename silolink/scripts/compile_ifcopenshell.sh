#!/bin/bash
# 在 Docker 中编译修改的 IfcOpenShell（silolink fork workflow）
# 使用增量构建（如果 build_fast 已存在）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "=== Compiling Modified IfcOpenShell in Docker ==="
echo ""

# 检查 Docker 镜像
if ! docker image inspect ifcopenshell-build >/dev/null 2>&1; then
    echo "ERROR: Docker image 'ifcopenshell-build' not found!"
    echo "Please run ./build_docker_image.sh first"
    exit 1
fi

# 检查源代码
if [ ! -f "$REPO_ROOT/src/ifcgeom/hybrid_kernel.h" ]; then
    echo "ERROR: Modified hybrid_kernel.h not found!"
    exit 1
fi

echo "Compiling IfcOpenShell with SIGSEGV recovery patch..."
echo "This may take 30-60 minutes on first build..."
echo ""

# 在 Docker 中编译
docker run --rm \
  -v "$REPO_ROOT:/build/ifcopenshell" \
  -w /build/ifcopenshell \
  ifcopenshell-build sh -c "
    set -e
    
    # 创建或使用现有的构建目录
    if [ -d build_fast ]; then
        echo 'Using existing build_fast directory (incremental build)...'
        cd build_fast
    else
        echo 'Creating new build_fast directory...'
        mkdir -p build_fast && cd build_fast
        cmake ../cmake \
          -DCMAKE_BUILD_TYPE=Release \
          -DCOLLADA_SUPPORT=OFF \
          -DGLTF_SUPPORT=ON \
          -DIFCXML_SUPPORT=OFF \
          -DPYTHON_EXECUTABLE=/usr/bin/python3 \
          -DCMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/opencascade
    fi
    
    echo 'Building ifcopenshell_wrapper (this may take a while)...'
    make -j4 ifcopenshell_wrapper
    
    echo ''
    echo '=== Build complete! ==='
    if [ -f ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so ]; then
        ls -lh ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so
        echo 'SUCCESS: Compiled .so file found!'
    else
        echo 'WARNING: Expected .so file not found in expected location'
        find . -name '*ifcopenshell_wrapper*.so' -ls
    fi
  "

echo ""
echo "=== Compilation complete! ==="
if [ -f "build_fast/ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so" ]; then
    echo "Compiled file: build_fast/ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so"
    ls -lh build_fast/ifcwrap/_ifcopenshell_wrapper.cpython-310-x86_64-linux-gnu.so
else
    echo "WARNING: Compiled .so file not found!"
    echo "Please check the build output above for errors."
    exit 1
fi



