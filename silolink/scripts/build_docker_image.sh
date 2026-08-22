#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE="${IMAGE:-ifcopenshell-build-py311-amd64}"

cd "$REPO_ROOT"

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: Docker is not installed" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "ERROR: Docker is not running" >&2
  exit 1
fi

docker build \
  --platform linux/amd64 \
  -f silolink/docker/Dockerfile.py311-build \
  -t "$IMAGE" \
  silolink/docker

docker image inspect "$IMAGE" >/dev/null
docker images "$IMAGE"
