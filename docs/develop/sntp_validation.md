# SNTP 配置与验证

日期：2026-10-10。

## 行为与配置

`SZPI application` 中的 `SNTP server source` 是互斥 choice：

- 固定地址：`CONFIG_SZPI_SNTP_SERVER_CUSTOM=y`，`CONFIG_SZPI_SNTP_SERVER_ADDRESS="pool.ntp.org"` 为默认值，可以替换为指定域名或 IP，不带 URL scheme / 端口。空地址或超过 253 字节的地址返回错误，不伪造校时成功。
- 网关地址：`CONFIG_SZPI_SNTP_SERVER_GATEWAY=y`，从经过 station / 当前连接校验的 GOT_IP 事件复制 STA 网关。无有效网关时返回错误并停止旧 SNTP 配置；不读取 AP 接口网关，不请求 DHCP option 42，不回退到公网服务器。路由器必须实际提供 NTP（UDP 123）。

服务器地址未变时 restart；地址变更先通过官方 `esp_netif_sntp_deinit()` 停止旧配置，再 init 新地址。初始化失败不标记 initialized，可由现有有限重试或下次连接重试。服务器地址由服务复制持有，IP 事件和局部字符串不作为长期借用对象。

由 runtime 的唯一 Wi-Fi task 调用 app 私有时间服务，不新增项目任务或同步对象。官方 SNTP 运行在 lwIP TCP/IP 依赖任务内。配网提交成功后仍等待配网服务收尾再启动校时。30 秒间隔、最多 3 次额外请求与后台 SNTP 轮询行为保持不变；时间同步 callback 才置位 TIME_SYNCED，UI 时区仍为中国标准时间。

## 主机测试

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Itests/time_sync_stubs -Itests/provision_stubs tests/test_szpi_time_sync.c -o /tmp/test_szpi_time_sync_custom
/tmp/test_szpi_time_sync_custom
cc -std=gnu11 -Wall -Wextra -Werror -DCONFIG_SZPI_SNTP_SERVER_GATEWAY=1 -Itests/time_sync_stubs -Itests/provision_stubs tests/test_szpi_time_sync.c -o /tmp/test_szpi_time_sync_gateway
/tmp/test_szpi_time_sync_gateway
cc -std=gnu11 -Wall -Wextra -Werror -DCONFIG_SZPI_SNTP_SERVER_ADDRESS='""' -Itests/time_sync_stubs -Itests/provision_stubs tests/test_szpi_time_sync.c -o /tmp/test_szpi_time_sync_empty
/tmp/test_szpi_time_sync_empty
```

三种主机测试通过。覆盖固定域名选择、初始化失败后再次初始化、地址未变时 restart、restart 错误返回、网关切换后 deinit / init、缺失网关停止旧服务、空地址拒绝，以及只有同步 callback 才设置时间有效位。使用模拟的官方 SNTP API，不代表真实网络校时验收。

## 固件与板上验证

- 默认固定地址模式 IDF 6.1 构建通过，生成 sdkconfig 中 `CONFIG_SZPI_SNTP_SERVER_CUSTOM=y` 和默认服务器生效。
- 网关模式使用独立 `/tmp/szpi-sntp-gateway-build` 构建及 `/tmp/szpi-sntp-gateway-config` 配置，避免修改项目默认模式；IDF 6.1 构建通过，网关 choice 生效。
- 未烧录或操作设备。板上待验证：指定 LAN IP / 公网域名校时、提供 NTP 的网关校时、网关变更后的目标更新、网关不提供 NTP 时的有限重试与 UI 状态、Wi-Fi 重连。
