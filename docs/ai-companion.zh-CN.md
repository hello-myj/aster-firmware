# 小鲸助手

鱼鹰光电 P4C5 和 FAKE POD NANO PRO 共用小鲸的离线唤醒、对话界面和 DeepSeek 客户端。两块板通过各自的麦克风和音频输出适配器采集、播报。

## 编译与配置

1. 在对应开发板的 ESP-IDF 环境和工程目录执行 `idf.py -DASTER_AI=ON build` 并烧录。新构建默认关闭 AI。已有构建可用 `-DASTER_AI=OFF` 关闭。开启时使用该板的 `dependencies.ai.lock`，将“你好小智”的 WakeNet 唤醒模型和 VADNet 人声检测模型编入应用分区。
2. 仅体验离线唤醒时，从桌面打开「小鲸助手」，点「开启 AI」，等待「请说『你好小智』」。
3. 要使用联网对话，先在设备上连接 Wi-Fi。在电脑上使用装有 `pyserial` 的 Python，从固件仓库运行下列命令，将端口替换为设备实际端口。程序会隐藏输入，也支持用 `--file` 指定仓库外的 UTF-8 密钥文件。

   ```sh
   python tools/ai_key.py --port COM11
   ```

4. 首次配置 Key 后默认使用「联网对话」，点击右上角按钮可切换回「离线唤醒」。开关和模式在设备上保存。清除 Key 可运行 `python tools/ai_key.py --port COM11 --clear`。

FAKE POD 的 AI 构建会加载 `sdkconfig.defaults.ai`。其中使用 Mbed TLS 的软件 AES，为同时运行的语音连接留出内部 DMA 内存。复用已生成的 `sdkconfig` 时，该文件中的设置优先。请在 `menuconfig` 中核对 WebSocket 缓冲为 4096 字节、SNTP 服务数为 2，关闭 Mbed TLS 的 Hardware AES。

密钥通过 USB 单独写入设备 NVS。固件、SD 卡和串口日志均不携带 Key。设备闪存中的凭据保护取决于产品的 NVS 与闪存加密配置。

### 讯飞 ASR 和火山 TTS

可通过串口为测试设备配置讯飞「语音听写大模型（中英）V2.0」和火山「豆包语音合成模型 2.0」。讯飞文件逐行写入带字段名的 `AppID`、`APIKey` 和 `APISecret`，火山文件写入带字段名的 `API Key`。文件保存在仓库外。

```sh
python tools/ai_key.py --port COM11 --xfyun /path/to/xfyun.txt --volc /path/to/volc.txt
```

可用 `--speaker` 指定豆包语音 2.0 音色，默认使用 `zh_female_vv_uranus_bigtts`。小鲸在本地检测到人声后开始上传 16 kHz PCM 分片，句末请求最终转写。回答仍由 `deepseek-flash` 生成，生成的文字逐段送入火山 WebSocket，与音频接收、播报并行。命令 `python tools/ai_key.py --port COM11 --clear-speech` 清除这两项服务凭据并恢复原语音组合。[讯飞 V2.0 API](https://www.xfyun.cn/doc/spark/spark_zh_iat_new.html) · [火山双向流式 TTS](https://docs.volcengine.com/docs/doubao-voice/bidirectional-streaming-text-to-speech-websocket?lang=zh)

这套语音组合将句末等待设为约 0.42 秒。在等待讯飞定稿时预建火山会话，收到转写后后台关闭识别连接，答复可立即开始生成。同一次对话内，后续提问会尝试复用已有的 DeepSeek HTTPS 连接。

录音任务只向本轮 PCM 缓冲追加样本，独立的上传任务按序从中发送。检测到人声后，设备也会后台预热首轮 DeepSeek 连接。到答复阶段若预热已完成就复用该连接，预热未完成时仍会直接发起模型请求。

录音期间识别连接中断时，本轮录音会继续保留在 PSRAM。检测到句末后，助手会通过新的讯飞连接重传一次。手动结束对话会取消重传。

## 提问与播报

- 在亮屏桌面或助手页说“你好小智”，也可点「直接提问」。看到「等待你开口」即可讲话，检测到人声后会显示「已检测到人声」。停顿约 0.7 秒后提交，也可点「说完了」。等待开口最多 10 秒，单次上传音频最多 15 秒。未检测到人声便结束本轮，录音留在设备上并释放。
- 一次唤醒可以连续聊多轮。播报结束后，屏幕显示「可以接着说」，此时可直接追问。小鲸保留本次会话最近四轮问答。安静 10 秒会结束会话、保留最后的回答，并回到唤醒待命。点「结束对话」、返回桌面或切换模式也可以结束会话。
- 聊天使用 DeepSeek HTTPS。默认语音组合使用 DeepSeek 识别和合成，配置讯飞和火山凭据后，对应语音请求改走各自的 WSS 接口。设备在离线唤醒阶段本地处理音频，只有进入联网提问才会上传。提问保存在 PSRAM，发送后释放，不写 SD 卡。回答从系统当前音频输出播放。
- 如果说话时 Wi-Fi 尚未取得地址，助手会保留这一句，在屏幕显示「正在连接 Wi-Fi」，等待已有连接或使用已保存的网络重连。连接失败时页面会提示前往 Wi-Fi 设置。
- 串口命令 `aster ui ai` 打开助手，`aster ui ai-status` 显示唤醒状态、联网模式、凭据和对话阶段。从桌面执行 `aster ui ai-mode cloud` 可开启联网助手并保存设置，`aster ui ai-mode offline` 可开启离线唤醒。已启用联网对话时，`aster ui ai-talk` 可从亮屏桌面或助手页发起对话。

## 实现

离线唤醒与提问采集分别由 WakeNet 和 `vadnet1_medium` 处理。录音任务按 VADNet 查询到的帧长送入 16 kHz 单声道样本。模型配置为连续人声 128 ms、连续静音 672 ms。首次检测到人声时保留最近 500 ms 的音频，以涵盖判定延迟和词首。参考了小智根据人声状态变化通知上层的做法。Aster 直接调用 ESP-SR 的 VADNet 接口，在一次提问结束后释放模型。[小智的语音状态处理](https://github.com/78/xiaozhi-esp32/blob/7f389c3c46ccd3df18d152c9d2f687cc723ea890/main/audio/engines/afe_audio_engine.cc#L450-L461) · [乐鑫 VADNet](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/vadnet/README.html)

麦克风以 16 kHz 采集单声道 PCM。在默认语音组合下，VAD 判断句末后，设备通过 `/v1/tools/asr` 提交完整的 WAV，收到本轮转写。与此同时，设备提前连接语音合成 WebSocket。DeepSeek 官方 ASR 每次返回一句完整的转写。[语音识别](https://api-docs.deepseek.com/zh-cn/guides/asr/)

聊天请求使用 `deepseek-flash` 和 `stream: true`。设备从 SSE 的 `delta.content` 读取新增文字，逐步更新屏幕，同时通过 `wss://api.deepseek.com/v1/tools/tts` 的 `text` 消息提交新增部分。一旦出现可朗读的首句或足够长的分句，就提前为这一段发送 `finish_text`。若还有后文，设备在聊天请求释放 TLS 后开启第二个合成连接，后文的 PCM 按顺序接在首句之后。短回答只开一个合成连接。语音服务通过二进制消息返回 24 kHz 单声道 PCM，设备检查服务端的合成结束状态。如果建立语音连接失败且尚未发送文字，会用 HTTPS 合成这一轮的完整回答。[流式聊天](https://api-docs.deepseek.com/zh-cn/api/create-chat-completion/) · [双向流式合成](https://api-docs.deepseek.com/zh-cn/guides/tts/)

设备先为音频输出预留 DMA，再启动云端请求。播放任务从 PSRAM 队列独立取音频，预缓冲 16 KiB（约 0.34 秒音频）后开播，更短的回复在合成结束后开播。队列按需分配 16 KiB 音频块，上限取开启对话时可用 PSRAM 的四分之一和 3 MiB 中的较小值，以便及时读完服务端突发返回的音频。播完后再次打开麦克风，等待追问。

可在已开启联网助手的桌面执行 `aster ui ai-bench`，用固定文字测量从模型开始回答到设备播放的时间。`aster ui ai-bench whole` 使用同一提示词并在模型答完时才结束整段合成输入，用于对照。两条命令均调用真实服务，绕过麦克风和 ASR。串口记录首段文字提交、模型完成、首包 PCM 和开播时间。

### 对比电脑与实机延迟

在电脑运行同一提示词、同一模型的基准测试。默认分别测一次冷连接和热连接，热连接使用 `GET /models` 预热。通过 `--runs` 调整每种模式的次数，`--chat-only` 可以只测聊天。电脑通过验证证书的直连接口访问服务，`--bind-ip`、`--dns-server` 和 Windows 下的 `--interface-index` 可指定与设备同网关的出口。

```sh
uv run tools/validation/ai_latency.py --key-file /path/to/key.txt --runs 3 --json timings.json
```

实机命令 `aster ui ai-bench warm` 使用相同的预热方式。比较串口的 `voice phase=chat`、`chat TLS ready`（新连接）、`chat headers sent`、`chat response headers` 和 `stream first token`。板端发送标记位于请求头写完时，电脑标记位于请求体写完时。发送到首字的区间包含剩余上传、网络往返和服务端处理。电脑收到 16 KiB PCM 的时间可与实机开播时间对照。

角色图在构建时生成 RGB565A8 资源，先存小端 RGB565 颜色平面，再存逐像素透明度。LVGL 可在 460×460 和 480×480 屏上放大并按条带重绘。设备工具的动态注册与权限分派仍属于后续功能。[DeepSeek 工具调用](https://api-docs.deepseek.com/zh-cn/guides/tool_calls/)
