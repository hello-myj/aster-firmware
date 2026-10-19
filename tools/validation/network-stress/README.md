# N4 公开 SDK 并发验收应用

这是验收负载，使用普通 APP 生命周期和公开 SDK。不是默认安装应用，也不改变连接/TLS/堆预算。素材和测试证书由生成器写到仓库外，不能直接编译这个缺少素材的源目录。

## 准备与运行

以下在固件仓库执行；替换电脑局域网地址、串口和输出路径。设备与电脑需互通，不能把 WSL 的回环地址当作设备可访问地址。测试前保存原 APP、前台、音量、网络及证书配置。

```sh
python tools/validation/prepare_network_stress.py \
  --output ../diagnostics/n4/app --host <设备可访问的IPv4> \
  --certs ../diagnostics/n4/certs
../aster-sdk/bin/aster build ../diagnostics/n4/app --target xtensa --aot
```

若源码 SDK 使用 `python tools/aster.py` 启动，则用该入口替代示例中的 `aster`。生成短门槛工程时额外传 `--fast`，与长测工程使用不同输出目录。短门槛要求至少 100 次 HTTP 完成/关闭和至少 10 次取消，结束后自动退出。

在电脑启动 `tools/validation/network_n4_peer.cjs CERT_DIR`；它复用本项目 HTTP/MQTT 测试对端，需 Node 和位于仓库外的 aedes 0.51.3。端口见对端文件头。WSL 用户用 Windows 原生 Node 提供可访问的 LAN 服务，按实际网络类型仅放行测试端口；结束后移除临时规则。不要把本地测试证书加入正式镜像。

将该板匹配的网络诊断固件刷入后，独占串口执行：

```sh
python tools/validation/network_n4_capture.py \
  --port <串口> --sdk-tools ../aster-sdk/tools \
  --package ../diagnostics/n4/app/dist/xtensa/aster.validation.networkstress.ast \
  --ca ../diagnostics/n4/certs/ca.pem --output ../diagnostics/n4/wave \
  --seconds 600 --width 320 --height 480 --gestures
```

LCKFB 使用对应串口与 `240×320`。短门槛捕获增加 `--fast`，传入短工程包。捕获器不刷机、不自动清理原 APP，不拼接中断前后时长；出现错误必须保留失败结果。长测结束保留测试画面，实体确认后再清理。

## 负载与边界

- 每 20 秒切换 TCP+UDP、HTTPS+UDP、MQTTS+HTTP，遵守两个共享连接和一个 TLS 配额。TCP 512B、UDP 64B、HTTP 16KiB 均校验内容；每第十次 HTTP 请求取消。只有未发送数据的 TCP 建连超时允许关闭并退避 1 秒后重试，单独记录超时/恢复次数，连续 3 次失败则终止；发送超时、内容错误等仍直接失败，不重放可能已送达的数据。
- 共享画布每帧重画完整背景（双缓冲不保留局部像素），持续图片/文字/命令与整帧绘制，定期重新加载图片；播放短 WAV 背景和音效，仅校验调用/状态，不自动断言扬声器发声。
- 只有同时声明加速度计和陀螺仪能力时才采样 IMU；未启用时记录为未覆盖，不能声称采样已验证。
- `N4LAT` 是 SDK 调用的毫秒时钟分桶，最大完成耗时覆盖 TCP/HTTP，MQTT 另有连接准备耗时。`N4NET bytes` 是已校验的接收字节总量，不含协议头，不是链路带宽。
- `CONCURPH t0/t1/t2` 和 `drawmax` 实际覆盖 FRAME 事件内网络、传感器、音频及绘制工作；不是纯光栅耗时或物理屏幕 FPS。UI 确认框暂停造成的事件间隔也应单独解释。
- 协议上下文 5ms 轮询只能给出观察峰值下界。静态 PSRAM 池预留不是活跃 APP 使用量；TLS 池内部峰值不能与其物理预留重复相加。首次 worker 栈常驻应与逐轮泄漏区分。

## 收尾

实际检查上拉确认/取消、下拉打开/收回、Wi-Fi 设置打开/返回和退出恢复；待后台静止后核对服务 `active/closing/psram`、协议池及内存。卸载临时 APP，移除上传包、测试 CA 和临时数据，恢复原应用/前台/音量/网络及该板日常固件，最后停止服务器、删除临时防火墙规则。原始日志、图片、JSON 和成品留仓库外，仅提交精简结论与可复用工具。

完整 SDK 解压后，可在相应宿主运行 `verify_network_devkit.py --sdk 解压目录/aster-sdk --output 仓库外目录`，核查文件哈希和 SDK/固件/模拟器映射，并通过包内工具创建、编译、验证独立 AOT 工程和执行 WASM。该检查不替代硬件验收。

短/长门槛完成后，先保留实体确认，再执行 `network_n4_lifecycle.py` 复查 APP 退出循环与 Wi-Fi 返回；它默认三轮分别等待约 3/23/43 秒后退出，覆盖网络轮换不同阶段的资源回收。所有原始串口及内存对照留在指定输出目录。结束保持测试 APP 已安装、已停止，仍需按上述收尾流程恢复设备。`summarize_network_n4.py CAPTURE_DIR --output LOCAL_JSON` 可汇总一次捕获，自动排除安装前旧会话的 stop 报告，保留毫秒直方图、超时恢复、负载吞吐和采样边界。

排查显示异常时，生成器可传 `--scene full` 固定完整场景，或 `low`/`transaction` 固定其他负载，网络协议仍按原节奏轮换。视觉对照不等于默认 mixed 场景的完整长时门槛。
