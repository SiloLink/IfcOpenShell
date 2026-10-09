## Silolink fork notes

这个目录用于隔离本 fork 相对 IfcOpenShell 上游仓库的构建内容，方便长期追踪 upstream 变更时减少冲突面。

### 目录结构

- `silolink/docker/`: silolink 的构建镜像
- `silolink/scripts/`: 本 fork 的辅助脚本（编译 / 打包 wheel）

### 快速开始：构建自定义 wheel（Linux py311）

目标是基于钉住的 upstream `261de82d5ab5653907dd216ae3e27384b86d4fda`（0.9.1）source 构建 `ifcopenshell==0.9.1+silolink.1`：

- Rocky Linux 9 builder
- upstream `nix/build-all.py`
- official Open CASCADE 7.8.1 shared dependency stack，使用 upstream packaging helpers 打包运行时依赖
- upstream `IfcOpenShell/build-outputs:rockylinux9-x64` dependency cache when available
- 当前 checkout 的 SiloLink source patches
- 完整 IFC schema 集合；Python wrapper 与全部 native plugins 使用 ABI v2 和 `IFOPSH_SAFE_INSTANCE=ON`

升级到 0.9.1 时必须清理旧的 IfcOpenShell build/install 产物并完整重编；第三方依赖缓存可以复用。

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
