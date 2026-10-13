# ABI 兼容与三仓库候选发布检查

本入口连接固件公开契约、固定 SDK、固定模拟器、AOT 工具链和 APP 制品。它不上传发布、不推送 Git、不替用户刷机，也不把主机测试当作实机验收。

## 兼容报告

`contract/abi-baseline.json` 是初始 ABI 3.7.0 的审阅基线；`tools/abi_compat.py` 用 SDK WASI Clang 的 wasm32 数据模型计算整数宽度、类型大小/对齐、字段大小/偏移，包含 event 的嵌套 union。报告同时比较导入名称、WAMR 签名、C 原型、枚举/数值宏及 `contract/package-format.json` 的 ASTP 线格式。编译器计算失败立即失败，不以正则猜布局或跳过字段。

```bash
python tools/abi_compat.py --clang ../aster-sdk/toolchain/wasi-sdk-24.0-x86_64-linux/bin/clang --baseline contract/abi-baseline.json --snapshot build/abi-candidate.json --report build/abi-report.json
```

报告分类：无机器可见变化仍需语义审查；新增类型/常量为可选增加；新增硬导入要求提高 ABI minor 和 APP 的 abi_min；移除符号、改签名、既有布局/整数宽度/枚举值或包格式变化视为破坏兼容。提高版本号或更新哈希不解除破坏兼容阻断，需另行设计新契约代际并验证迁移。

PR 工作流优先读取 base commit 的基线，避免候选同时改基线掩盖破坏；首次引入基线需人工审阅。发布分支应要求该 PR 检查成功。不要在普通接口变更中重新生成基线来消除差异；直接 push 绕过 PR 治理也不能视为已完成兼容审查。

结构兼容不证明语义兼容。每次公开行为变化需添加行为回归和人工审阅；能力查询只处理符号已存在但设备能力缺失，不能防止加载时硬导入不存在符号。

## 固定组合与必需制品

`contract/release-matrix.json` 固定 SDK/模拟器完整提交号、工具链 profile 与 catalog 哈希、发布必需包名。固件使用本次 CI 候选提交，报告记录三个实际提交、脏状态、修改文件哈希、WASI Clang 与 wamrc 哈希/版本、Node 版本、字体资源和 APP 包哈希。修改 SDK 或模拟器兼容组合后，应先提交各仓库，再更新矩阵到经过验证的新提交；本地未提交改动会明确标记 dirty，不能作为正式发布。

发布必需示例为 SDK 2048、IMU 立方体和固定打砖块源码。打砖块不属于 SDK 原生示例目录，完整参考源码保存在 `tests/fixtures/compatibility/breakout-source/`；来源文件哈希在同目录 manifest 中。APP 缺失、构建失败、契约/字体不符或测试失败均阻断，不静默 skip。

`tests/fixtures/compatibility/aster.hello.ast` 和 `aster.game.2048.ast` 由指定旧 SDK Git revision 的源代码构建并冻结，包含 Xtensa AOT；普通验证不重新生成它们。manifest 记录 SDK 提交、编译器 profile/hash、包哈希。新固件 C parser 校验旧包，模拟器实际执行其 WASM；Xtensa AOT 仍需目标设备验证。打砖块包是独立固定参考，并非有独立 Git 提交号的旧 SDK 项目。

## 本地与 CI

准备 Python 3.12 环境（安装 SDK requirements 和固件 requirements-ci.txt）、C 编译器/CMake/Ninja、Node 22.20.0；在模拟器执行 `npm ci`。三个仓库并列时，从固件目录执行：

```bash
python tools/release_check.py --sdk ../aster-sdk --sim ../aster-sim --output build/release-check
```

输出目录必须是新的，以免使用上次生成的示例或工程。脚本执行契约/文档/字体检查、POSIX host 构建、固件和 SDK 单元测试、[快速入门](quickstart.md)中的真实命令、必需 AOT 示例构建、全部模拟器测试，以及冻结旧包的校验和实际模拟运行。`--node` 可指定 Node 22 可执行文件；正式发布加 `--require-clean`。不满足矩阵的源码 revision 或工具链哈希会失败，不能自动升级依赖来绕过。

`.github/workflows/release-validation.yml` 在 push、PR 和手动触发时读取矩阵检出其他仓库，执行全链路检查；通过后并行完整构建立创和微雪固件，应用 `patches/` 里的固定补丁，上传每块板的镜像、配置和 identity。Zephyr revision 由 west.yml 锁定，SDK 版本取该 revision 的 SDK_VERSION；构建 identity 保存实际工具链缓存。发布必须等待 integration 和两板 firmware job 全部成功，不能只下载其中一个 artifact。

SDK 自己的 CI 上传显式 xtensa 示例包；模拟器 CI 检出它声明的 SDK revision，构建并运行 2048/IMU。单仓库模拟器测试允许没有外部包时跑单元测试，不能充当完整发布检查；完整发布入口提供三份必需包并要求零跳过。

CI 上传集成 report、日志、快速入门截图、全部必需包及字体资源；失败也上传已生成报告。报告中的 hardware=not_run 是真实状态。远端 CI 没有执行时，不把工作流文件存在写成 CI 已通过。

## 实机部署与验收

连接设备，确认没有正在运行的 APP；测试旧包会升级设备上同 ID 的 hello 和 2048，保留 data/cache。快速入门使用独立 test.quickstart，已有同 ID 会拒绝覆盖，结束后卸载它。可使用：

```bash
python tools/compat_hardware.py --sdk ../aster-sdk --port /dev/ttyACM0 --quickstart build/release-check/quickstart-app/dist/xtensa/test.quickstart.ast --firmware-sha256 FIRMWARE_SHA256 --output build/compat-hardware
```

镜像哈希应来自本轮已确认刷入的镜像；参数不是设备读回证明。微雪已禁用 IMU，不能强行删除 requires 或假造能力来通过安装。LCKFB、Windows 原生工具和用户可见画面仍分别验收。长期记录放在仓库 `docs/validation/`，下一次测试记录提交/镜像、板型、配置、包哈希及结果。

## 2026-10-13 本地完成记录

P1 第 5～7 项实现和本地验收已完成。固件 65 项、SDK 75 项测试通过且没有跳过；新包和冻结旧包通过真实固件 C parser；模拟器 38 项通过、零跳过。新独立工程的创建、AOT 构建、校验和截图命令实际执行成功；微雪上冻结旧 hello/2048 AOT 及该工程真实部署、启动、退出通过，测试工程已卸载。立创、微雪均完成完整固件构建；本次未刷新设备固件，AOT 验证基于本会话上一版已刷入的镜像，报告中记录其哈希来源。

证据保存在 [validation/p1-2026-10-13/](validation/p1-2026-10-13/)。本地包含未提交改动，report 标记 dirty；这不是已发布版本。两板 build identity 的 hardware_test=not_run 指本次新构建镜像没有刷入实机，不与另行记录的旧镜像 APP 验证混为一谈。

后续三仓库分支提交/推送、本地 `--require-clean` 及 LCKFB 旧包/新工程补测已完成，见下方更新。仍待发布前完成：核实远端 CI 的 integration 及两板 job 成功；Windows 原生快速入门；用户可见画面和外设行为按设备范围验收。最新板型保护改动尚未提交，提交后应对最终候选重新运行干净发布检查。P0 剩余 SD/断电/长期资源验收仍在 [可靠性清单](reliability-validation.md)，不会因 P1 自动检查通过而清除。

## LCKFB 与分支更新（2026-10-13）

三仓库已提交并推送 GitHub 的 work/p0-p2-20261013 分支，主分支未合并；本地 --require-clean 检查已通过。远端 CI 是否通过仍需单独核实，Windows 原生验收仍待完成。

LCKFB 纯板型诊断镜像上，冻结旧 hello/2048 和文档快速入门工程均安装并以 Xtensa AOT 启动通过；hello 正常结束，2048 与快速入门运行后停止。临时 hello/快速入门已卸载，2048 同 ID 更新保留原用户目录。证据见 [LCKFB 兼容结果](validation/lckfb-2026-10-13/compatibility/hardware.json)。记录中镜像哈希是调用者提供的已刷入镜像哈希，并非设备读回认证。构建板型保护和本轮记录为后续未提交改动，不将旧分支 HEAD 宣称为它们的完整源码版本。
