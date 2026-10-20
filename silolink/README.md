## Silolink fork notes

这个目录用于隔离本 fork 相对 IfcOpenShell 上游仓库的构建内容，方便长期追踪 upstream 变更时减少冲突面。

### 目录结构

- `silolink/docker/`: silolink 的构建镜像
- `silolink/scripts/`: 本 fork 的辅助脚本（编译 / 打包 wheel）

### 快速开始：构建自定义 wheel（Linux py311）

目标是基于钉住的 upstream `261de82d5ab5653907dd216ae3e27384b86d4fda`（0.9.1）source 构建 `ifcopenshell==0.9.1+silolink.2`：

- Rocky Linux 9 builder，产物要求 GLIBC 2.34+（`manylinux_2_34_x86_64`）
- upstream `nix/build-all.py`
- official Open CASCADE 7.8.1 shared dependency stack，使用 upstream packaging helpers 打包运行时依赖
- 使用 auditwheel repair 补齐非 manylinux 系统库并校验目标平台兼容性
- upstream `IfcOpenShell/build-outputs:rockylinux9-x64` dependency cache when available
- 当前 checkout 的 SiloLink source patches
- 完整 IFC schema 集合；Python wrapper 与全部 native plugins 使用 ABI v2 和 `IFOPSH_SAFE_INSTANCE=ON`

升级到 0.9.1 时必须清理旧的 IfcOpenShell build/install 产物并完整重编；第三方依赖缓存可以复用。

`.2` 修复无效曲面映射的空指针访问、退化面导致的整组带样式几何丢失，
并保留开放曲面及其开洞，不再强制将其缝合为实体。普通转换失败仍可切换
兼容内核；`SIGSEGV` / `SIGBUS` 等致命错误交由调用方的进程隔离处理，
不在受损进程中继续尝试内核。
混合内核向子内核传递模型解析后的精度和单位，优先尝试兼容颜色及开洞要求
的完整转换；全部失败后才在独立内核中保留有效子几何。直接转换和多线程
使用相同策略，失败尝试的输出及部分结果缓存不会污染后续尝试，不改变
模型精度、输出格式或插件 ABI。
单次使用的映射几何也保留实例的开洞上下文；开洞转换失败不会发布未切孔
或部分处理的几何。

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
