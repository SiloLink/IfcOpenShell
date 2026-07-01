#!/bin/bash
set -euo pipefail

cat >&2 <<'EOF'
compile_ifcopenshell.sh is obsolete.

Use ./silolink/scripts/build_py311_amd64_wheel.sh instead. The py311 wheel
script now compiles IfcOpenShell through upstream nix/build-all.py with the
official Rocky/static dependency stack before packaging the wheel.
EOF
exit 2
