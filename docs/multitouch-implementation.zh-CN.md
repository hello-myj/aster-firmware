# 双触点输入实施与验收记录（代码及分项实机验证完成，完整稳定性验收待完成）

起始日期：2026-10-19；交付整理：2026-10-21。用户要求先写方案，再由 GPT-6 luna、极高推理强度实现。固件、SDK、模拟器开发分支均为 `feature/multitouch`；分支起点分别为 `ccf8e3b`、`9ae3733`、`632fa8e`，公共 ABI 为 **4.0.0**。2026-10-21 用户明确要求将当前多指版本提交上传并合并到各仓库主分支，取代本文早期“不提交/不推送/不合并”的阶段约束；不在本次创建 Release 或发布 SDK 包。

当前交付范围为双触点驱动、状态恢复、UI手势隔离、SDK说明及游戏回归、模拟器多指支持。两板基础组合操作有实机反馈，微雪最新恢复修正通过40轮驱动故障重放和日常交互复查；LCKFB最新恢复逻辑只有构建验证，尚未重新刷机验收。微雪A3报文来源、其他偶发整机卡死、完整自然双指压力/交叉轨迹等仍未全部排除，不能把合并主分支等同M4全部通过。以下保留各阶段的原始状态与失败记录；原始串口日志、截图和构建产物不入库。

## 目标与范围

接通微雪3.5B（AXS15231B）、LCKFB实战派S3（FT6336）两点触摸，保持触点身份、独立按下/移动/释放、事件与轮询一致，使 SDK 的 `apps/neon-duel` 能在屏幕上同时“方向＋攻击”“方向＋跳跃”。两点是当前硬件上限，不宣称五指。缩放/旋转识别器不在本轮范围；先让APP获得可靠原始触点。

既有单指launcher、LVGL、调音台、返回确认仍须正常，不能将两个手指交替喂给同一个鼠标导致坐标跳跃或误退出。保留现有显示、I2C/IMU、音频、网络优化。驱动及输入状态使用固定有界存储，不新增线程或动态触摸堆。

芯片能力依据（不等同屏模组实测）：

- AXS15231B数据手册第7页Touch声明two-point detection及60Hz：[原厂手册](https://files.waveshare.com/wiki/common/AXS15231B_Datasheet_V0.5.pdf)。微雪官方例程给出14字节报文及逐点偏移`6*i+2..5`、event位和12位坐标；其解析器将Y高字节上半部屏蔽，没有读取ID。ARCS源码命名`ID_POS=4`，但与微雪例程对该半字节的处理存在差异，见M1限制。
- FT6336系列具备两点能力；[FT6336U原厂手册](https://files.waveshare.com/wiki/common/FT6336U_Datasheet.pdf)及[立创S3板级资料](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/)。实际板型/屏幕固件是否稳定输出两点，留待分别实测。

## 实施前阻断点（已在本分支修复，AXS身份字段仍待实机确认）

1. `platform/esp32s3/input_axs15231b.c`读取14字节，允许count<=2，但只解析第一点。
2. LCKFB节点`ft6336@38`使用`focaltech,ft5336`；本机Zephyr的`drivers/input/input_ft5336.c`明确忽略其余点。不能只改平台数组。
3. `platform/esp32s3/input.c`只有一个pt，边沿暂存、状态快照、调试注入都单点；`aster_touch_evt_t`无slot。
4. `systems/zephyr/runtime/aster_runtime.c`在LVGL和headless两路都要复制slot；当前公共`aster_event_t.u.touch.slot`已有字段，不能漏其中一路。
5. `aster_native.c:n_input_touch`当前每点写16字节（x/y/action/slot），但公共`aster_touch_point_t`为12字节且sim也按12字节写入，是已有契约差异；不可直接给APP数组追加slot破坏布局。
6. `gesture.c`、launcher/flux、LVGL只接受单指流；既有注释错误宣称FT芯片只有单点，需要更新。
7. sim的Host.touch、浏览器touching布尔、replay及snapshot都只支持单点。
8. `neon-duel/src/main.c`已有slots[8]并按event.slot合并held，但需验证每指释放不影响另一指，暂停/恢复不粘键；其旧“ABI3.7”日志应纠正。

## M1 驱动与平台（代码、主机测试、两板构建完成；AXS身份字段和实机待验）

先核实官方报文格式，记录可信来源和解释。AXS逐点解析，FT读取点数及两条记录，保留hardware tracking ID，再映射为稳定的逻辑slot0/1。报文排列交换不得导致触点互换；ID可大于1，不能直接拿硬件ID索引2元素数组。ID复用必须先结束旧生命周期，不能给旧指发MOVE。

优先把LCKFB双触点驱动放在固件自身`platform/esp32s3/`并用专属compatible/Kconfig/CMake/设备树选择，避免本轮成果留在仓库外Zephyr未提交补丁里，也避免旧驱动和新驱动重复实例化。核实已有Zephyr input multitouch协议支持情况，选择原子帧提交/slot同步方案；不允许在多条X/Y事件之间暴露半更新坐标。

共同处理：0/1/2点、显式release和消失ID、非法count/id/坐标、I2C错误。瞬时读取失败不是“全部手指抬起”的可靠证据；当前策略连续5次或累计120ms读错时释放已按下slot，然后等待全UP/零触点帧或可确认的新DOWN epoch；后者要求所有活动点均为明确的新DOWN，单纯CONTACT不能解除恢复等待。设备断开/无能力时清零状态。驱动每20ms读取一次，不新增线程。

平台内部事件新增slot，固定两触点状态和有界队列；保留DOWN原始位置、MOVE合并以及UP，慢消费者不能丢DOWN/UP导致粘键。溢出需明确可恢复策略与测试；不同slot不得互相覆盖。区分最大触点数和一次poll最多事件数（两指完整快点可能多于2个事件），调用者小批次读取须能排空。

旧单指注入仍默认slot0；扩展有界slot注入。合成slot不会被周期空硬件帧释放。AXS原始报文诊断由`CONFIG_ASTER_AXS15231B_RAW_DIAG`显式开启，默认关闭；临时设为`y`后，两点持续报文每50帧（20ms轮询下约1秒）记录候选ID及完整14字节帧，串口日志标签为`AXS two-contact candidate IDs`和`AXS two-contact raw frame`。此诊断没有单独shell命令；已有`hw touch [n]`只采样平台slot事件，不输出控制器原始报文。不能把合成双点当真机双指已验证。

`touch_protocol.c`实际FT/AXS解码、`touch_state.c`双slot队列和`touch_ui.c`分别编入C sanitizer测试。FT6336输出延续旧Zephyr驱动的(col,row)转置以匹配现有校准。AXS解析暂按ARCS的record byte 4高半字节为ID，使用其值分配slot而不依赖记录顺序；Waveshare BSP把该字段视为保留位，来源存在冲突，只有抓到微雪实机原始双点帧后才能确认身份稳定。当前没有将该项写为硬件通过。

## M2 固件运行时、UI与契约（代码与主机测试完成；实机待验）

公开事件布局不变，传递已有slot。事件和快照逐点完成raw→logical→APP映射；当前scale-fit输入映射会把letterbox点夹到最近的APP边缘并返回成功，保持这一既有行为。快照按slot逐点压缩；若映射接口拒绝其中一点，不丢掉另一点。`max_points`反映当前硬件/能力，不以全局数组容量虚报。

公共轮询既有结构保持12字节x/y/action，按它修正固件校验/写入，max=0不写、max=1不越界、max=2写两条；用缓冲哨兵与ASan证明。slot身份通过既有事件获取；如果需要带slot快照，应新增版本化接口而非偷偷改变旧结构，本轮优先不新增ABI入口。更新契约注释并按现有export/import工具同步SDK、lock、sim兼容指纹；若需要提高minor版按现有发布规则统一处理，不擅自改主版本。保留并测试旧包/导入签名/结构尺寸。

单指UI采用稳定primary owner：一次接触期间不因另一指坐标而切换，也不能把副指UP变成鼠标UP；主指释放后若副指仍按住，抑制接管直到全部释放再开始新UI手势。APP仍获得两指完整事件。

系统边缘手势：第二点加入时，取消当前尚未触发的系统单指手势，抑制到全部释放；不要把副指滑动识别成调音台/退出。系统面板打开期间通过既有APP pause/resume回调清理捕获，并抑制触摸直到全部释放；关闭面板后的resume在下一批输入交付前完成；不增加公开touch action或事件布局。LVGL pointer收到多指时先取消LVGL捕获，避免抬起误报点击。应用触摸继续传递两个slot。

ABI 4.0公共轮询保持`aster_touch_point_t`的12字节x/y/action布局；slot仅通过既有touch event传递。native桥使用临时4-u32平台快照，再按guest 12字节结构逐点`memcpy`写入，支持非对齐guest地址。max=0不访问输出，能力上限为当前板最大触点数。

`tests/test_touch_multitouch.py`、`test_input_coalescing.py`、`test_input_snapshot_layout.py`、`test_exit_confirm.py`覆盖协议/slot身份、队列慢消费/合成输入、legacy单指延迟DOWN/MOVE/UP、12字节轮询max=0/1/2与保护字、完整延迟双指手势隔离、单指捕获取消、退出取消确认、手动暂停与面板关闭后的resume顺序。均使用实际C实现；全固件套件结果在最后验收记录更新。

## M3 SDK游戏与模拟器（代码、双平台主机与APP回放完成；实体待验）

SDK `apps/neon-duel`原游戏规则/像素画风不重做，验证slot分开held、任意先后释放、移动到按钮外、pause/resume/切页后清键。若APP现有slot逻辑足够，只补必要修复和有意义的真实输入组合测试；不增加只测试硬编码实现的冗余断言。构建同一`--target xtensa --aot`包，供两板及sim用。

模拟器：Host按slot保存两点、公开事件带slot；浏览器Pointer Events映射pointerId到两个稳定slot，使用pointer capture，支持cancel/lostcapture/窗口失焦全部释放，默认鼠标slot0。replay允许可选slot并默认0，校验范围，支持同时间两指记录；暂停/停止清所有点。旧单指消息与回放仍可用。不能把键盘方向＋攻击成功当触摸双指成功。

同步公开输入说明、SDK开发手册、游戏操作、模拟器说明，明确原始多点与高级手势的区别，以及两点能力的实际验收状态。

## M4 自动化与实体验收（主机测试及两板配置构建完成；实体待验）

自动测试至少覆盖：

- 真实两点报文解析、count0/1/2、第二点、ID乱序/复用、显式release/消失、重复帧、边界坐标、非法长度/id/count、读错误/恢复。
- 按A→按B→抬A→B移动→抬B及反序、同时按/抬、两指交叉不换slot、两指短按全部挤在慢绘制间隔、队列满后可恢复。
- legacy单指DOWN-origin/MOVE/UP、每次只poll1条、快照max0/1/2及内存哨兵、触摸禁用、旋转/letterbox两指分别映射、越界UP清理。
- 两指不会误开上下拉；主指与副指释放顺序不令UI跳坐标，全部释放后单指手势恢复；APP暂停/返回取消/恢复不粘键。
- sim pointerId/slot映射、取消/失焦、旧replay兼容、新双指replay、霓虹对决同时held和独立释放。

主机执行各仓库适用测试、契约/文档检查；新增输入功能必须做实际C路径或实际Host的测试，不能仅grep源码。两板纯配置构建，记录配置、提交、通过/失败、静态RAM增量和测试限制。测试日志、屏幕图、原始报文、证书、包和镜像在仓库外`../diagnostics/multitouch-20261019/`，不进Git。无需下载或编译整套ESP-IDF依赖来编译APP。

实体顺序：先诊断页显示两个不同颜色触点/slot，同时移动、交换位置、逐个抬起；再部署霓虹对决，在TRAIN模式持续方向＋出拳/踢、跳跃＋攻击，释放其中一指不影响另一指；检查单指launcher、上下拉/返回确认、取消/恢复和IMU共享总线。两板分别做，不把一个模组的结果当另一个通过。

刷机前采集型号、原应用/存档、音量/网络和已知日常镜像；串口独占。确认设备与纯板配置对应，避免错刷。保持待验APP前台直到用户实体反馈，不能切走再问。遇无硬件继续主机/构建工作并列明待测；没有实体确认不能标全部完成。清理测试包/日志，恢复用户环境，释放串口。

## 执行规则与进度维护

各阶段完成代码与检查后更新本文件状态，区分“代码完成/构建通过/两板实体通过”；只在整阶段约定验收完成后标题加“（已完成）”。GPT-6 luna负责依据本方案修改代码和验证、报告实际修改文件及剩余问题；主代理复核安全、兼容和资源边界，再协同用户实机验收。允许合理调整实现细节，若改变契约/手势规则要在文档记录理由。所有改动留在feature/multitouch，当前不提交、不推送、不合并；用户再要求提交时用中文。

### 当前验收记录（2026-10-19）

- 固件主机套件：`python -m unittest discover -s tests -v`，128项通过；日志在仓库外`../diagnostics/multitouch-20261019/firmware-tests-final.log`。
- LCKFB构建：`PATH=/home/myj/zephyrproject/.venv/bin:$PATH python tools/firmware.py build --config config/default.conf --build-dir build/multitouch-lckfb`，目标契约`lckfb-s3`通过；日志`../diagnostics/multitouch-20261019/build-lckfb-final.log`。
- Waveshare构建：`PATH=/home/myj/zephyrproject/.venv/bin:$PATH python tools/firmware.py build --config boards/waveshare_35b/qspi-prefetch.conf --build-dir build/multitouch-wave`，目标契约`waveshare-35b`通过；日志`../diagnostics/multitouch-20261019/build-wave-final.log`。
- AXS原始帧诊断默认关闭（`CONFIG_ASTER_AXS15231B_RAW_DIAG=n`）；仅需现场抓取ID候选字段时，临时向构建配置加入`CONFIG_ASTER_AXS15231B_RAW_DIAG=y`。没有对应CLI子命令，也不应把该开关留在日常镜像配置。
- 诊断启用分支单独用临时配置编译通过：`build/multitouch-wave-raw-diag`，日志`../diagnostics/multitouch-20261019/build-wave-raw-diag.log`；临时配置文件也留在该诊断目录。普通两板镜像配置确认诊断关闭。
- 对比各自现有release基线，`.dram0.bss`分别增加336B（LCKFB）和360B（Waveshare）；其他静态DRAM段未见变化。构建输出DRAM总占用分别为193736B和192128B（区域大小399108B）。产物都在独立`build/multitouch-*`目录，不覆盖常用构建。
- 主机测试验证slot边沿/快照、迟到消费的完整双指序列仲裁、两指不同抬起顺序与全部抬起后恢复单指、12字节ABI轮询max=0/1/2和保护字、未对齐guest地址、合成DOWN经过周期真实空帧仍保持到MOVE/UP，以及面板关闭后先resume再交付新DOWN。未刷机；AXS ID半字节仍需微雪实机抓包确认，FT坐标转置仅由旧驱动/校准路径和主机测试确认。

### SDK / 模拟器接续验收

- SDK全套106项通过（`sdk-tests.txt`）。霓虹对决原有slot控制无需重构，新增`tests/test_neon_duel_input.py`将实际生产触摸处理分支和战斗引擎编入ASan/UBSan检查；覆盖方向+出拳、反序方向+踢、跳跃+攻击、独立抬起和暂停/恢复清键。修正旧ABI日志，未改变游戏规则或画风。
- 同一个`apps/neon-duel/dist/xtensa/com.example.neonduel.ast`包含AOT与WASM，已编译验证。源码回放`apps/neon-duel/tests/two-touch.replay.json`运行160步（含10步暂停），Linux与Windows原生Node均正常结束，150帧/150次present。这不是实体屏或面板FPS验证。
- 模拟器新增4项多指测试和2项worker退出测试，专项均通过、无跳过。双平台完整主机测试各106项，其中87通过、19跳过、0失败：未配置的外部网络/Scene包等可选集成及停用的旧接口用例未执行，不能称全套零跳过或发布验收完成。另编译真实2048/IMU包参与旧单指/传感器回归。
- Windows完整游戏回放曾在正常结束后以0xC0000005异常退出；主分支对照通过。本分支改为收到done后等待worker自行退出（最多2秒），超时/看门狗/错误仍强制终止；相关测试与修正后完整游戏回放通过。该结果支持正常收尾方式的修正，不代表已查明原生库内部崩溃根因。失败记录保留在仓库外，后续不得抹为首次即通过。
- 公共ABI仍4.0；SDK仅更正输入结构说明及对应lock hash，模拟器契约快照已核对当前SDK工作区，标明工作区身份，不冒充发布提交。独立SDK仍无需固件源码或ESP-IDF库来构建APP。未刷机、未提交、未推送、未合并；两个屏模组的双点ID/方向/组合操作和系统UI实体回归仍待分别完成。

继续实体检查时先确认原环境和板型，使用本轮独立构建目录。当前接入LCKFB ABI4.0，原24个APP均STOPPED、音量74、Wi-Fi保持；已安装霓虹对决1.0.1，基线在仓库外`device-baseline.txt`。微雪尚未接入本轮验证。

### LCKFB 首次刷入与安装（2026-10-19，游戏双指实体验证通过）

按用户要求，已核实`/dev/ttyUSB0`为LCKFB，再以`west flash --skip-rebuild -d build/multitouch-lckfb --esp-device /dev/ttyUSB0`刷入本分支固件，写入校验通过。SDK单串口部署`com.example.neonduel.ast`成功（同版升级保留原数据），日志确认ABI4.0、320×240、AOT、RUNNING；已进入TRAIN。原24个应用保留、音量74，游戏持续前台。屏幕抓取确认训练画面与底部控制正常显示；截图只能证明渲染，不能替代实体双指验证。原始日志和截图仅在仓库外`diagnostics/multitouch-20261019/`。当前未测试微雪，也未提交或合并。

用户实体反馈：霓虹对决 TRAIN 中“双指组合及分别释放都正常”。本项确认一指方向与另一指攻击同时操作、松开攻击指不释放方向、全抬后无持续移动/攻击。LCKFB 的本轮游戏双指验收通过；不据此推断微雪也通过。双指交叉ID、系统上下拉与返回取消、共享I2C/IMU并发等完整M4实体回归仍应分别记录，目前没有这些新增实体反馈。继续保持霓虹对决前台，未提交/推送/合并。

### 微雪首次刷入与安装（2026-10-19，等待实体反馈与原始ID核对）

同时存在两块板时，已先核对PONG和`aster sys info`，确认`/dev/ttyACM0`为微雪、`/dev/ttyUSB0`及`/dev/ttyACM1`为LCKFB通道。微雪原20个APP均STOPPED、音量31、Wi-Fi已连接。刷入独立`build/multitouch-wave-raw-diag`镜像并校验成功，安装同一个霓虹对决AOT包，当前21个APP。日志确认ABI4.0、480×320横屏、RUNNING；屏幕抓取确认TRAIN模式，游戏保持前台。微雪注入选择TRAIN使用当前显示坐标315,187，不能直接复用LCKFB的210,140。

原始报文只读采集写入仓库外`../diagnostics/multitouch-20261019/wave-raw-touch.txt`，本次采集进程最长15分钟。已请用户持续双指至少2秒并验证组合、独立释放；目前尚无本轮实体反馈，不能把诊断日志尚未出现视为通过。取得证据后还需关闭采集，刷回`build/multitouch-wave`（RAW_DIAG关闭），恢复游戏前台并验证日常配置。原有应用、音量和联网配置保留，未提交/推送/合并。

### 微雪中途失去响应／退出的调查（尚未验收通过）

用户反馈“刚才都是好的，现在卡住了”。现场串口仍可查询，霓虹对决RUNNING且err=0；内核堆可用65524B、libc池可用48356B，外部堆最大空闲块至少5MiB，未见崩溃或内存耗尽证据。触摸日志持续报告解析EINVAL。补充诊断后抓到完整14字节均为`a3`，读取调用没有返回I2C错误；这不能证明有效触点ID，更不能据此通过双指验收。证据在仓库外`wave-stuck-state.txt`、`wave-raw-touch.txt`、`wave-rejected-raw.txt`。

用户再次反馈“强行退出了”，日志在57.485秒记录`runtime: right-edge return`，随后释放应用缓存并回主页。复查发现多指UI仲裁只重置通用手势，遗漏独立的FLUX右边缘返回状态。已增加私有`aster_launcher_app_touch_cancel()`并在仲裁reset时调用，防止旧的边缘起点跨多指序列残留；新增真实生产函数测试覆盖取消后松手不退出、新单指仍可返回。此修正不能解释全部A3异常，也不能代替微雪复测。

诊断开关开启时额外输出每50个连续拒绝帧的完整报文，并预填接收缓冲0xCD以区分未写入与真实重复数据；同一诊断分支尝试I2C控制器总线恢复并记录返回值。总线恢复实验仅限RAW_DIAG构建，不作为已经验证的日常故障恢复方案。后续须核对异常是否消失、ID是否稳定，再决定生产修复，不能将实验当完成。

### 全链路接续验证（2026-10-20，进行中）

用户确认手势修正后的TRAIN中“操作正常，没有卡住或退出”。原始双指帧抓到`00 02 81 17 00 72 5f 60 81 27 11 71 60 61`及相邻帧，候选ID为0/1，已作为真实解析回归样本；这不等同于交叉移动/报文换序已验证。随后仍有全A3帧；重复调用`i2c_recover_bus()`返回0却未持续消除异常，因此已删除该诊断恢复调用，不把返回0当修复成功，不把A3猜成“正常空闲”或直接当全部松手。诊断预填0xCD后仍读回A3，排除了接收缓冲完全没被写入的猜测。来源仍待整链路对照。

新增两项状态处理：仅显式UP的完整有效帧也能解除错误恢复等待（不只接受count=0；含活动点/未知事件的帧不解除），真实总线读错不会取消只有合成触点的CLI操作。实际C sanitizer测试覆盖这两条行为；它们不证明I2C已恢复。

增加`hw touch slot <0|1> <down|move|up> <x> <y>`和不消费事件的`hw touch state`，保持原单指命令。新CLI检查slot、操作、参数数量和数值；旧无触点`up`不再读取未初始化坐标。坐标使用当前显示逻辑空间，输出每个活动slot与最终active数量。主机测试编译实际命令处理函数，覆盖有效slot、非法数值/动作/缺参及空快照释放。

按用户要求执行**整条链路的实机对照**，不能止于状态机或主机测试：

1. 先核对板型与应用，临时启用原始触摸诊断，捕获I2C返回结果和报文；不改GPIO/屏幕时钟，也不刷错另一块板。
2. 普通APP前台时运行`tools/validation/multitouch_capture.py --port <port> --sdk-tools ../aster-sdk/tools --log <仓库外日志> --app aster.gfxshow --rounds 20`。过程中手不要碰玻璃；覆盖两种slot先后、两种释放顺序、持有/移动/分别松手、右边缘起点不误退出，每步读快照并查APP RUNNING。该组绕过芯片/I2C，仅是下游对照，脚本成功不算全链路通过。开始前停止其他前台APP。
3. 同一固件/同一APP进行真实双指按下、移动、逐个松手、全松后等5秒重新操作，抓原始双点ID与平台快照；确认异常A3是否伴随平台停止响应。再换回霓虹对决检查组合；单指上下拉/取消恢复也要检查。需要用户实体操作配合，命令无法伪造芯片生成真实两指数据。
4. 若注入也失控，定位状态机/映射/手势/生命周期；若仅实体失败，继续查总线/触摸驱动/硬件，并保留原始数据。两者可能同时有问题，不将排除某层的证据夸大为唯一根因。
5. 关闭RAW_DIAG刷回日常固件，保持待验APP前台，再确认物理操作。未实测的项目明确待测，不把一次“操作正常”替代完整验收。

目前微雪USB从WSL消失，刷日常镜像在打开`/dev/ttyACM0`前失败，未写入；LCKFB仍连接，未挪用其端口。已请用户重新接入微雪。全部修改未提交、未推送、未合并。

### 微雪重接后的注入对照结果（2026-10-20）

重新核实`/dev/ttyACM0`为微雪ABI4.0，用户期间前台改为Snake、音量92；以这一新现场作为恢复基线，不恢复成此前31。刷入本轮独立诊断固件，启动Gfx Show并完成20轮实机注入：142次slot命令（含最终释放）、81次不消费事件的快照检查、21次APP状态查询，全部通过。两种按下顺序、两种释放顺序、持有第二指后第一指移动、右边缘返回取消均按预期；全松active=0，APP一直RUNNING。日志`wave-cli-20-rounds.txt`、结果`wave-cli-20-result.txt`均在仓库外。该段未触发连续无效报文警告；不能据此说芯片/I2C通过，因为输入为合成。

为了补足APP交付证据，RAW_DIAG另增加有效count/event/ID变化的原始帧（最多每100ms一条）以及实际`deliver_event()`后的APP DOWN/UP日志。重新刷入后首次脚本启动太早，SD尚未挂载、应用注册数为0，前置检查失败，未进入注入；此为脚本启动时机问题，已加最多30秒等待SD和应用注册就绪，保留失败日志`wave-cli-fulltrace.txt`。重跑2轮通过，并抓到两个slot各自完整DOWN/UP、quit=0；日志`wave-cli-fulltrace-ready.txt`。新增trace仅诊断构建存在，未改变公开ABI。

目前Gfx Show保持前台，停止注入，只读采集原始I2C触摸报文/APP事件，并每2秒查询平台快照、每10秒核对APP状态，记录到`wave-physical-fullchain.txt`。已请用户双指按住/移动/逐个松手、全松等5秒再单指操作。**这一轮真实触摸全链路反馈尚待收到，不能标记硬件稳定性通过；日常无诊断固件也尚未恢复。** 串口由该采集进程独占，最长15分钟，后续指令前应先停止采集。

### 命令行自动操作复测与新修正（2026-10-20）

用户纠正上一条选择：并非“应用意外退出”，实际是“正常”；不把该误选记为崩溃/误退出复现。用户要求后续由代理用CLI操作屏幕，不再要求用户配合操作。已停止周期采集，独占串口执行自动触摸、APP切换、快照及上下拉检查。此前实体日志确认单指返回确认/取消、下拉/收回，未见该段双点原始帧；不能用主观“正常”补齐双点交叉等缺失证据。Gfx Show持续超过13分钟RUNNING，期间全A3报文仍出现，但不能由此认定硬件损坏或判定其为正常空闲协议。

自动UI脚本实际复现一次上拉失败：`hw touch drag`使用无符号`(y2-y)`/`(x2-x)`插值，向上/向左时下溢，MOVE坐标可能超屏，只有最终UP正确；UP本身不会触发拖动识别。已用int64有符号差值计算中间点，真实命令处理函数测试验证向左上、向右下8个MOVE及最终UP坐标。失败证据保留`wave-command-ui.txt`，不归因于触摸硬件或吞掉失败记录。

修正后诊断固件自动操作Gfx Show、Snake各3轮，通过双指按下/移动、两种释放顺序、全松active=0、右边缘手势取消、上拉显示返回确认/点取消、下拉打开及收回、覆盖层关闭后APP收到新的DOWN。日志`wave-gfx-ui-fixed.txt`、`wave-snake-ui-fixed.txt`；Gfx Show截图渲染正常。另验证Snake双指保持时CLI暂停，暂停中分别释放，再恢复后新触摸交付成功，记录`wave-command-lifecycle.txt`。所有步骤由CLI完成，不依赖用户手势。

可复用脚本增加`--overlays`（每轮确认/取消、下拉/关闭，交替测试上拉收回与再次下拉收回）及`--trace`（要求恢复后的实际APP DOWN诊断日志）。仍需明确：命令注入在同一实机上覆盖输入处理、UI与APP，真实触摸驱动同时运行；命令本身无法制造电容触点或证明芯片在所有实体双指情况下正常。未据此把完整M4硬件验收标题改成已完成。

130项固件主机测试通过，两板配置构建通过。日常微雪构建确认RAW_DIAG关闭，正在刷回并做最终自动复测；随后保留Snake和原音量92。全部修改仍在feature/multitouch，未提交/推送/合并，所有测试图和原始日志留在仓库外。

日常恢复已完成：`build/multitouch-wave`刷入和校验成功，RAW_DIAG关闭、未保留实验性总线恢复调用。Snake再完成3轮CLI双指+返回确认/取消+调音台开关与上拉收回，全部通过（`wave-daily-command-final.txt`）。终态确认Snake RUNNING、全部触点active=0、音量92、21个应用仍在；共享串口在截图/查询完成后释放。此前20轮纯双指、两APP各3轮覆盖层组合、暂停/恢复及这3轮日常复测均由代理执行；不能据此声称CLI制造了真实电容双指信号。A3报文来源仍未查明，保留为后续协议/总线调查项，不标硬件损坏，也不标M4全部完成。

### 间歇性无响应的进一步修正（2026-10-20）

用户继续报告双指后偶尔系统卡死或触摸无响应，并补充“多触摸几次又恢复”。因此此前单轮/纯注入通过不能作为问题已经解决的证据。现场仍可查询串口和线程；sysworkq 4096B栈实际使用1524B、剩余2572B，APP工作栈使用约52%，未见该现场的栈耗尽证据。此信息不排除未捕获的其他整机卡死。

复现了一条明确的软件失效路径：有效按下后出现连续错误，恢复逻辑强制释放并置`ignore_until_clear`；如果控制器的UP/零触点帧漏读，后续新按下也被无条件丢弃，只有以后碰巧收到的全UP/空帧才解锁。实际生产C测试在修正前断言失败（`lost-up-before-fix.log`），吻合多次触摸后才恢复的现象，但不宣称是所有卡死的唯一原因。

内部触点样本新增`fresh_down`，解码器保留事件0与CONTACT事件2的区别；公开ABI、事件布局和SDK接口不变。错误恢复现在接受完整全UP/空帧，或可明确识别的新DOWN epoch：所有活动点都必须是新DOWN，允许同帧旧UP；旧CONTACT、未知状态，或“新指DOWN + 旧指CONTACT”仍不能重开。实际测试覆盖单点/双点重新按下、混合新旧触点不解锁和有界释放，修正后通过。若新的DOWN本身也漏读，这个保守规则仍可能等待下一次明确边沿；不通过猜测A3为空闲或盲目超时来误造按下。

增加仅RAW_DIAG可用的`hw touch replay`：在真实AXS工作队列轮询中，照常完成I2C读操作后，用30帧有界fixture覆盖本轮报文，序列为双指DOWN→6帧A3错误且不发UP→旧CONTACT→新的双指DOWN→双UP→空帧。它不是物理电容信号，不作为芯片已通过的证据；但经过实际解码器、驱动错误处理、平台队列、运行时及真实APP。普通固件不含重放入口和状态。

微雪在Gfx Show、Snake各20轮此驱动故障重放通过：每slot实际APP事件严格为DOWN/UP/DOWN/UP，旧CONTACT阶段无新DOWN，最终active=0且APP仍RUNNING，共40轮/1200个fixture驱动采样。日志`wave-driver-replay-isolated.txt`、`wave-snake-driver-replay.txt`。首次运行中，重放结束后的真实触摸也进入同一串口片段，使固定事件数量断言失败；已保留`wave-driver-replay.txt`，按fixture开始/完成边界限定统计，不能把真实事件混入的失败误报成输入失控。

`hw touch state`增加非破坏性健康快照：`recovery`（等待恢复标志）、`errors`（连续错误）、`pending`（待消费事件）、硬件/合成触点mask。未来现场active=0时可区分无触点与被恢复门禁阻挡，无需先多点几次破坏现场。无新增线程、堆分配或公开ABI。正在恢复新修正的日常固件并复查普通交互；A3来源、未复现的整机卡死以及完整自然触摸压力仍不标完成。

新恢复逻辑的日常微雪固件已刷入，RAW_DIAG关闭，Gfx Show再完成3轮双指与覆盖层恢复。19次健康快照的recovery均为0，最后全部触点释放，音量92，Gfx Show保持前台；日志`wave-recovery-daily-check.txt`和`wave-recovery-daily-final-state.txt`。验证脚本在有健康字段的固件上同时要求recovery=0，不能再把合成触点能动误当真实输入已解锁。130项主机测试通过、两板构建通过；本轮只刷微雪，LCKFB新恢复逻辑尚未重新刷入实测。未提交/推送/合并，不能将这条确定修复升级为“所有偶发卡死已消除”。
