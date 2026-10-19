# HTTP/HTTPS

HTTP 与 TCP/UDP 共用两个连接槽。HTTPS 与 MQTTS、WSS 共用一个 TLS 名额。完成、失败或取消后的句柄仍占名额，调用 close 后释放。

```text
aster net http start GET http://<主机>:9080/fixed?n=4096 30000 1048576
aster net http start HEAD http://<主机>:9080/fixed?n=4096 30000 1048576
aster net http start POST http://<主机>:9080/echo 30000 1048576 hello
aster net http result <句柄>
aster net http read <句柄> 1200
aster net http header <句柄> X-Repeat 0
aster net http cancel <句柄>
aster net http close <句柄>
aster net http stats
aster net http ca /SD:/ca.pem
aster net http ca default
```

`start` 的参数依次是方法、URL、总超时毫秒、响应体上限，以及可选的文本请求体。没有请求体但要带头部文件时，用 `-` 占位。Host、Content-Length、Transfer-Encoding、Connection、Accept-Encoding、Expect、Upgrade 由服务填写。

`start` 返回句柄只表示已排队。state：1 连接、2 TLS、3 发送、4 等响应头、5 响应体、6 完成、7 失败、8 取消。`read` 返回正数是字节数，0 是读完，未就绪时再查。HTTP 404/500 仍是传输完成，状态码在 status 里。

请求体最多 2048 字节，URL 最多 1024 字节，头部最多 2048 字节 / 32 个字段，响应队列 2048 字节。重定向最多 3 次，拒绝从 HTTPS 降到 HTTP。默认信任根是 ISRG Root X1。`ca` 只影响本次开机的 HTTPS，不影响应用包签名。

测试服务：

```bash
node tools/validation/network_http_server.cjs
```

HTTP 端口 9080。需要 HTTPS 夹具时，把证书目录传给该脚本。
