## Silolink fork notes

这个目录用于**隔离**本 fork 相对 IfcOpenShell 上游仓库的“自定义内容”（脚本 / Docker / 文档 / patch），方便长期追踪 upstream 变更时减少冲突面。

### 目录结构

- `silolink/docker/`: silolink 的构建镜像
- `silolink/scripts/`: 本 fork 的辅助脚本（编译 / 打包 wheel）
- `silolink/docs/`: 关键改动说明、复现与验证流程
- `silolink/patches/`: 以 patch 形式保存的上游源码改动（便于审阅/PR/移植）

### 快速开始：构建自定义 wheel（Linux py311）

目标是对齐官方 PyPI `ifcopenshell==0.8.5` 的 native build stack：

- Rocky Linux 9 builder
- upstream `nix/build-all.py`
- official Open CASCADE 7.8.1 static dependency stack
- upstream `IfcOpenShell/build-outputs:rockylinux9-x64` dependency cache when available
- current `silolink/main` source patches

前置：
- 已安装并启动 Docker

步骤：

```bash
./silolink/scripts/build_docker_image.sh
./silolink/scripts/build_py311_amd64_wheel.sh
```

产物通常在：
- `wheels/ifcopenshell-*.whl`

Cloud Build:

```bash
gcloud builds submit --config silolink/cloudbuild.py311-wheel.yaml .
```

Sanity check without running the long build:

```bash
bash silolink/scripts/test_py311_official_build_script.sh
```

The legacy `build_wheel.sh` delegates to the py311 script. The old split
`compile_ifcopenshell.sh` flow is intentionally disabled so we do not
accidentally rebuild against distro OpenCascade packages again.

