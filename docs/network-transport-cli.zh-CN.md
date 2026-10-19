# TCP/UDP

设备 shell 的 `aster net` 用来诊断连接。应用使用 ABI 4.0 的 `aster_net_*` 导入，不使用这些 shell 错误码。

先在系统设置里连上网络，再查地址：

```text
aster net status
aster net stats
aster net tcp connect --host <主机> --port 9000 --timeout-ms 5000
aster net result <句柄>
aster net tcp send <句柄> --hex 000102ff
aster net tcp recv <句柄> --max-bytes 128
aster net cancel <句柄>
aster net close <句柄>

aster net udp open
aster net udp sendto <句柄> --host <主机> --port 9001 --hex 000102ff
aster net udp recvfrom <句柄> --max-bytes 1200
aster net close <句柄>
```

命令只提交任务。句柄以 `NET handle=` 的输出为准。TCP 超时范围是 1..30000 ms。一次发送最多 96 字节十六进制，接收最多 1200 字节。`--hex -` 表示空数据。

全设备两个连接槽。终态句柄仍然占槽，`close` 之后才释放。只支持 IPv4 单播。

TCP 是字节流。`recv` 容量必须大于 0；读到 0 表示对端关闭，之后不能再发送。UDP 数据报最大 1200 字节，容量不够时返回所需长度并保留原包。

主机检查：

```bash
python -m unittest discover -s tests -p test_net_service.py
```
