## Silolink fork notes

这个目录用于隔离本 fork 相对 IfcOpenShell 上游仓库的构建内容，方便长期追踪 upstream 变更时减少冲突面。

### 目录结构

- `silolink/docker/`: silolink 的构建镜像
- `silolink/scripts/`: 本 fork 的辅助脚本（编译 / 打包 wheel）

### 快速开始：构建自定义 wheel（Linux py311）

目标是基于钉住的 upstream v0.9.0 source 构建 `ifcopenshell==0.9.0+silolink.1`：

- Rocky Linux 9 builder
- upstream `nix/build-all.py`
- official Open CASCADE 7.8.1 static dependency stack
- upstream `IfcOpenShell/build-outputs:rockylinux9-x64` dependency cache when available
- 当前 checkout 的 SiloLink source patches

前置：
- 已安装并启动 Docker
- 已运行 `git submodule update --init --recursive`

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
