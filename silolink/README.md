## Silolink fork notes

这个目录用于隔离本 fork 相对 IfcOpenShell 上游仓库的构建内容，方便长期追踪 upstream 变更时减少冲突面。

### 目录结构

- `silolink/docker/`: silolink 的构建镜像
- `silolink/scripts/`: 本 fork 的辅助脚本（编译 / 打包 wheel）

### 快速开始：构建自定义 wheel（Linux py311）

目标是基于钉住的 upstream `261de82d5ab5653907dd216ae3e27384b86d4fda`（0.9.1）source 构建 `ifcopenshell==0.9.1+silolink.6`：

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
模型精度或输出格式。

`.3` 在完整转换尝试结束后保留可用的局部结果：失败的洞口切割被跳过，
有效宿主和成功切割保留；布尔减法要求有效的第一操作数，失败的后续
操作数及其残留结果不会参与计算。逐面材质无法完整保留时可退回构件级
或默认材质。洞口切割始终要求完整的切割体，避免用部分切割体过度切除宿主。

重复 STEP 编号仅在定义内容一致时去重；冲突定义仍拒绝。最终缺失或降级
与成功回退分别产生带 STEP 编号的结构化日志，供 Parser 汇总。ShapeFix
循环、映射实例覆盖和文件加载异常的 Python GIL 恢复也包含在此版本中。
单次使用的映射几何保留实例的开洞上下文。

`.4` 修正 CGAL external-structure 重建异常时的 point locator 所有权：
重建成功前保留原 locator，失败时释放临时副本，避免析构访问已释放内存。
修正在构建目录生成独立头文件，不修改系统 CGAL；不改变几何精度或降级
策略，也不把失败的布尔运算视为完整成功。

`.5` 增加内部设置 `opening-recovery-mode`（`inline` / `defer` / `individual`）
和元素上的待恢复开洞标识：批量开洞失败后，`defer` 返回未切割的有效宿主及
待恢复的开洞与 representation 编号，`individual` 只做逐个切割，供 Parser 在
独立、可终止的进程中按宿主限时重试。能力名 `deferred_opening_recovery_v1`。
默认 `inline` 行为、几何精度、材质和诊断词汇不变。

`.6` 保留完整转换中均匀的逐面颜色和透明度，即使内核没有逐面材质数组，
也使用原始面样式覆盖对象样式；混合材质和不完整来源仍按既有规则诊断。

Python wrapper、geometry/parse 库和 native plugins 必须从同一源码一起
构建、打包；不可把不同源码修订的二进制、内部 C++ 类型或库混用。
版本号表示打包目标，具体源码提交及 wheel 哈希由交付记录绑定。

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
