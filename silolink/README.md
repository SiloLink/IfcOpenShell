## Silolink fork notes

这个目录用于**隔离**本 fork 相对 IfcOpenShell 上游仓库的“自定义内容”（脚本 / Docker / 文档 / patch），方便长期追踪 upstream 变更时减少冲突面。

### 目录结构

- `silolink/docker/`: silolink 的构建镜像（例如 `Dockerfile.build`）
- `silolink/scripts/`: 本 fork 的辅助脚本（编译 / 打包 wheel）
- `silolink/docs/`: 关键改动说明、复现与验证流程
- `silolink/patches/`: 以 patch 形式保存的上游源码改动（便于审阅/PR/移植）

### 快速开始：构建自定义 wheel（Linux）

前置：
- 已安装并启动 Docker

步骤：

```bash
./silolink/scripts/build_docker_image.sh
./silolink/scripts/compile_ifcopenshell.sh
./silolink/scripts/build_wheel.sh
```

产物通常在：
- `wheels/ifcopenshell-*.whl`

更多细节见：
- `silolink/docs/SIGSEGV_FIX_WORKFLOW.md`


