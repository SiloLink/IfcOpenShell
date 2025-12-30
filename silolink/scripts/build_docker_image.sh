#!/bin/bash
# 构建 IfcOpenShell Docker 镜像（silolink fork workflow）
# 注意：Docker build context 必须是仓库根目录（需要 COPY 全仓库源码）

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "=== Building IfcOpenShell Docker Image ==="
echo "This may take 10-20 minutes on first build..."
echo ""

# 检查 Docker
if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: Docker is not installed!"
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo "ERROR: Docker is not running!"
    echo "Please start Docker and try again."
    exit 1
fi

# 检查 Dockerfile
DOCKERFILE="$REPO_ROOT/silolink/docker/Dockerfile.build"
if [ ! -f "$DOCKERFILE" ]; then
    echo "ERROR: $DOCKERFILE not found!"
    exit 1
fi

# 构建镜像
echo "Building Docker image 'ifcopenshell-build'..."
echo "This will install all build dependencies..."
echo ""

docker build -f "$DOCKERFILE" -t ifcopenshell-build . 2>&1 | tee docker_build.log

# 检查构建结果
if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo ""
    echo "ERROR: Docker build failed!"
    echo "Check docker_build.log for details"
    exit 1
fi

echo ""
echo "=== Docker image built successfully! ==="
echo "Image: ifcopenshell-build"
docker images ifcopenshell-build



