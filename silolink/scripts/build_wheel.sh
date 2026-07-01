#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "build_wheel.sh is now an alias for the official-aligned py311 wheel build."
exec "$SCRIPT_DIR/build_py311_amd64_wheel.sh" "$@"
