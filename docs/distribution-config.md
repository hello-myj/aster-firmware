# 下载源与 TLS 证书配置

鱼鹰光电 P4C5、FAKE POD NANO PRO（ESP-IDF）及立创实战派（Zephyr）从同一份[分发配置](../config/distribution.json)生成默认源和信任根。默认地址指向华为云 OBS 的 `aster-source.json`。设备先读此入口，再按板型和架构读取同一快照内的文件。

## 固件默认值

修改 `config/distribution.json`：

```json
{
  "source_url": "https://example.com/aster-source.json",
  "ca_bundle": "certificates/roots.pem"
}
```

`ca_bundle` 相对该 JSON 所在目录解析，也可使用绝对路径。它是公开的 PEM
CA 证书集合，不是上传访问密钥，也不应含私钥。配置生成器会检查 URL 长度、
协议、凭据及查询参数，使用 TLS 库校验 PEM 格式，配置无效则构建失败。

默认 PEM 保留 ISRG Root X1、USERTrust ECC、GlobalSign Root CA - R3，支持现有
GitHub 和 OBS 下载。替换时应保留仍在使用的服务所需的信任根。

也可以保留仓库默认配置，构建时传入
`-DASTER_DISTRIBUTION_CONFIG=/absolute/path/distribution.json`（CMake 缓存参数）。
生成的头文件和证书字符串只写入构建目录，配置文件和 PEM 修改会触发重新配置。
首次开机界面、共享初始化逻辑和 HTTPS 服务使用同一配置，两套系统没有各自的地址副本。

## 商城默认值

`aster-apps/AppStore/app.toml` 的 `[build].defines` 中配置 `STORE_URL`。
`AppStore/tools/build.py` 默认读取它，也支持 `--server-url` 临时覆盖。
正式发布时，发布工具把目标渠道地址传给构建脚本。
商城不单独携带证书，使用固件的 HTTPS 服务和信任根。

## 已有设备优先级

- 固件：设备保存的资源源地址优先于构建默认值。
- 商城：设备保存的 `store.url` 优先于应用构建默认值。
- 首次配网页面可填写资源服务器地址；商城设置可保存新地址，无需重新编译。
- 在立创实战派、鱼鹰 P4C5 和 FAKE POD 上，用 `aster sys setup-source URL` 更换资源源并校验、补齐依赖。
  用 `aster sys setup-status` 查询进度和错误码。鱼鹰 P4C5 和 FAKE POD 也兼容原有的 `aster ui` 写法。

变更内置默认值或内置证书仍需要重新构建和烧录；这不是在线证书更新机制。
现有 HTTP CLI 的 `aster net http ca PATH` 可做本次开机的自定义 CA 诊断，
不等同于永久更新固件信任库。始终保留证书链、域名和有效期验证。
