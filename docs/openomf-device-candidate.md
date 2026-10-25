# OpenOMF 实机验证候选（2026-10-24）

状态：按用户明确要求跳过备份，已刷入USB控制台配置并通过写入校验；板型、ABI、SD和320×480原生主页截图已确认正常。OpenOMF 0.2.11 已以4MiB线性预算完成双Nova/ARENA4及后续场景对战，180秒结束RUNNING/err0；实测约2.85–5.37FPS，流畅度仍未达标。

## 可核对产物

- 设备：微雪 ESP32-S3-Touch-LCD-3.5B，8MiB PSRAM / 16MiB Flash；当前只读查询ABI4.1、SD已挂载、25个APP均停止。
- 固件：build/waveshare-openomf/zephyr/zephyr.bin，1503204B，SHA256 666314cd7276e0d27b7557a2a0b5c3c3eee269b49f7521a014cfdc7a33cfbb95。使用boards/waveshare_35b/usb-debug.conf，已核对控制台和shell均为usb_serial。
- 游戏：OpenOMF0.2.3，2217620B，SHA256 e0ede3e57721f2fc3d028a78333aabc2d3c8ecb285cf2a02bcbe0de765cea7c2；Xtensa AOT，31个公开Aster导入，通过包校验。

## 内存修改

固件调用WAMR load_ex并请求模块拥有需要保留的数据，成功且runtime明确允许释放时，回收原始AOT镜像；XIP模块保留原始后备缓冲。大AOT的临时加载区按后续线性内存加显示余量预留，释放后保留连续内存洞，避免AOT指令分配把大块PSRAM切碎。WAMR失败路径同步复制数据的所有权，防止失败加载泄漏。原ABI4.1和旧APP结构不变。

APP从12MiB线性内存收缩到3.5MiB；精灵采用无损压缩常驻、128KiB解码LRU，像素碰撞使用同一份解码像素，可修改表面先转为普通自有副本。保持原招式、碰撞和AI。SOUNDS.DAT只常驻索引和单样本缓冲，音效仍由原调度器同步提交到Aster缓存。

## 已有运行证据与待验证内容

0.2.2的4MiB版本已由用户启动原版DEMO，跨多个竞技场运行，记录C堆峰值2,796,544B（约2.67MiB），精灵节省约0.9–1.6MiB。它不代表全部机器人、锦标赛和结局的最高值。

0.2.3的3.5MiB版本已构建、包校验通过，音效改为SD按需读取后仍需运行验证，不能用0.2.2的数据假装它已通过。实机首要检查AOT加载成功、连续内存及余量、画面触摸、对战帧率、SD音效读取停顿和原版锦标赛。

## 设备操作与恢复

用户已明确选择跳过备份，直接烧录。未创建 Flash 恢复镜像；实际操作与验证见下节。APP 包和原始游戏资源均需部署，单独安装 AST 不能运行。

## 实际烧录记录

用户明确要求“不用备份，直接烧录”。首次使用默认selection.conf构建，烧录校验通过，但控制台在UART0，USB不能回应。发现配置选择错误后，改用usb-debug.conf重新构建、核对DTS控制台为usb_serial，再次写入0x0并通过esptool Hash校验。重启后USB查询返回waveshare35b、ABI4.1.0、8MiB PSRAM、16MiB Flash、SD mounted、25个已安装APP；原生主页截图已保存为build/openomf-usb-home.png。

未创建Flash备份，未执行整片擦除；本轮写入固件镜像区域。游戏资源使用公开SDK串口协议逐文件CRC校验上传，部署记录在OpenOMF/artifacts/device/deploy.log。最终游戏运行结果以实际加载日志和截图为准。

## 实机游戏启动结果

0.2.3 已完成 AOT 加载和输入镜像回收，但语言解析中大量短距离 fseek 导致 event 超过 5000ms，固件按既有看门狗策略重启。0.2.4 在 APP 的 sd_reader 中加入固定 8192B 缓存与逻辑游标，短距离跳转不再触发 SD 寻址，原版语言算法和文件内容不变；完整语言阶段约 495ms，全部七个初始化阶段成功。

0.2.4 包 2218632B（WASM468166B、AOT1742884B），31 个公开 Aster 导入，SDK 校验通过。69 个上传文件（68 个资源加 AST）均经过 CRC 校验；复用已验证资源后更新包安装成功。安装约 35 秒，部署脚本重新打开共享串口并将安装超时延长至 180 秒，避免原每次 30 秒截止导致丢失应答。

启动日志确认 AOT 指令区1522272B、原输入镜像1742884B已回收、线性上限56页，APP状态 RUNNING / err=0。25秒观察内原版片头实际120帧/20171ms，约5.95 FPS；stats中的 fps=60 是申请帧率，不能当作实际值。C堆峰值744752B，sys mem最大连续空闲块至少524288B。原版片头截图 build/openomf-device-024.png 为原始320×480屏幕缓冲（横屏内容在原始方向下旋转），不能当作已通过对战验证。

设备持续报告 AXS15231B 触摸无效帧，仍需用户实际触摸确认。原版对战峰值、音效听感、触摸和长时稳定性尚待实机验证。
## 0.2.6 DEMO 退出修复与实机对战

用户反馈 DEMO 黑屏回主页后，连续串口捕获确认从 VS 转竞技场时 event 超过5000ms，触发既有系统恢复；未出现 OOM。0.2.5 保留原始稀疏精灵编码并按需解码，但合并加载仍超时。0.2.6 将原版背景、两份 HAR、arena_create 拆分到独立事件，在加载完成前冻结逻辑，不放宽固件看门狗。私有堆由每次从头 first-fit 改为 next-fit，合并空闲块时同步更新游标。

实际 ARENA1 加载背景242ms，HAR1 784ms，HAR2 605ms，arena 初始化142ms，均rc=0。用户确认已进入对战。固件、ABI、线性上限保持不变；原版AI、招式、碰撞、机关没有删减。运行300秒末状态RUNNING/err0，C堆观察峰值1741472B。截图build/openomf-device-026-fight.png为实际机器人对战。

RGB565输出改成对齐32位写入，仅清除上下黑边，片头像素转换由7644ms/120帧降至3303ms/120帧。对战完整样本120帧/44095ms（2.72FPS），其中logic25720ms、draw7609ms、finish10498ms；C堆与Host/固件占用不同，不能把C堆峰值当总RAM。仍在进一步拆分耗时、优化绘制及音效转换，不能宣称已经流畅可玩。
## 最终0.2.11状态（2026-10-25）

3.5MiB/2.5MiB私有堆候选在跨角色加载和连续运行中仍有分配失败，0.2.8日志包括calloc(1,10998)，0.2.10中间包包括calloc(25192,1)。原版精灵数据现由运行时接管、解析对象置空；私有分配器最终采用按地址排序空闲链表及相邻合并，未继续保留next-fit。

最终0.2.11：线性初始/最大4194304B，C池3145728B，Host堆393216B，栈32768B；包2239356B，SHA256 b4005170ac51bb75389d15597199f4cb1b6fa87cbcf2b03ecc961107da43e5f3，31个公开Aster导入且包校验通过。双FIGHTR10.AF/Nova与ARENA4已实际加载，HAR871/886ms、初始化100ms。180秒结束RUNNING/err0，并已进入后续竞技场；C堆观察峰值2491376B、线性观测3960832B，最大连续外部块至少524288B、6块150KiB可同时分配。上述不是全流程最高值。

最重双Nova音效样本120帧/42055ms（2.85FPS），PCM194次/13774ms；后续ARENA1样本120帧/22360ms（5.37FPS）。画面及音效仍需原生性能改进，不能宣称完整移植验收或流畅。SDK stats的60是申请帧率。最终截图build/openomf-device-0211-fight.png；一次性诊断标记已消费，SD目录检查不再有device-demo.once。

明确--cpu=esp32s3的AOT与SDK现有产物SHA256完全相同，故CPU参数没有作为收益项或SDK新改动。下一步原生PCM加载/声像和批量索引绘制的公开能力方案见APP docs/SDK音频补齐方案.md，不表示这些能力已实施。
## 2026-10-25 ABI4.2 内存音效与 APP0.2.14

当前固件由usb-debug.conf构建，保留ABI4.1接口布局，新增audio-effects-memory bit22和aster_audio_effect_load_memory。验证Guest整个WAV范围及长度，平台解析PCM16格式、8–48kHz、单/双声道、WAV最多69632B和PCM最多65536B；返回前复制到Host自有内存，与文件加载共享四个句柄。没有保留Guest指针。SDK契约/头文件/能力表/手册及模拟器同步。这是本地候选，未作为官方SDK发布。

0.2.12取消临时WAV的SD写入与读回；0.2.13 CRT使用有范围检查的WASM bulk memory；0.2.14在3MiB私有池内增加可回收的256000B原画面背景展开缓存。APP固定4MiB线性内存，原AI/招式/机关保留，31个公开导入且没有libc外部导入。0.2.14实机180秒结束RUNNING/err0，跨ARENA4到ARENA0，观察C堆峰值2799760B；120帧样本约4.0–6.3FPS，仍不足以达到原版流畅体验。最大连续外部空闲块至少524288B，6块150KiB可同时分配，不是全流程余量保证。

内存音效固件镜像1503268B，SHA256 d87fab901c83e2fb7165588dc3a83ff5edd148115c48c068910612c8bc5b6d9b；flash-audio-memory-final.log记录烧录Hash确认。旧ABI4.1 APP曾在该固件启动。动态声像和独立BGM控制尚未补齐。

横屏异步双缓冲实验实际显示正常，用户反馈更顺畅；但present仍约43ms，增加614400B常驻缓冲，最大连续块降至262144B。因此display.c与runtime头文件的异步改动已回退，不能将其列为确定性能收益。实验日志build/openomf-rotated-async.log，差异保存在build/rotated-flip-experiment.patch。

### 原生大块拷贝实验（已撤回）

仅在Xtensa AOT内部对齐、互不重叠且至少64B的memmove增加32位拷贝，保留原范围检查和重叠回退；objdump确认实际l32i/s32i且没有逐字memw。候选SHA256 8ca385ad0bd7fdf331a3324627c439a4318dc45ebd43ecd736fa253d06f75072，esptool写入Hash通过。

同一0.2.14双Nova/ARENA4原版DEMO样本，draw约5828–6187ms/120帧，present约5.2秒/120帧，未比原固件显示明确收益；复杂动作使logic明显波动，不能根据单段总FPS认定拷贝加速。已恢复aot_memmove原实现，不保留这项无确定收益的固件改动。记录在build/openomf-word-copy.log和build/aot-copy-native.asm。

最终恢复镜像SHA256与d87fab90基线完全一致，flash-final-0214.log确认写入Hash。再次运行原版双Nova/ARENA4约60秒，258帧，结束RUNNING/err0；两个120帧样本25326ms（4.74FPS）和31615ms（3.80FPS），复杂动作logic波动较大。最大连续块至少524288B，6块150KiB可分配。当前近期实测约3.8–6.3FPS，不以早期单段接近6FPS宣称全程稳定。截图build/openomf-device-0214-final.png，原始面板缓冲320×480包含旋转后的横屏内容。
