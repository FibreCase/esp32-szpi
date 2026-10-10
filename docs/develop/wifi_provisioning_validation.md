# Wi-Fi 配网验证

实现范围、状态机、安全限制见 [Wi-Fi 配网设计](wifi_provisioning.md)。日期：2026-10-08。

## 已完成

- ESP-IDF 6.1 / esp32s3 构建：DPP、LVGL QR、确定性 gzip portal、锁定的 cJSON 依赖可编译链接。最终增量构建和全新独立 defaults 构建均通过；全新配置确认 DPP / QR 已启用、LVGL pool 128KiB、sys_evt 栈 4096B。
- Linux simulator 构建，128KiB LVGL 内存池下 Network / 方法 / 热点 QR / DPP QR 页 headless 绘制，重复 create/destroy 成功。64KiB 在新增页面创建时触发分配断言，因此提高至 128KiB；不是依靠编译推断内存够用。
- `test_szpi_provision_protocol`：二维码特殊字符转义、容量不足、SSID / 密码边界、64 位 hex PSK、WPA3限制；DNS A、AAAA、截断、压缩问题、容量不足及 20,000 个畸形随机报文。
- `test_szpi_wifi_service`：新候选不提前覆盖旧配置、旧 DHCP / 未核对目标不提交、成功延迟保留热点、重复 DHCP 不重复保存、NVS 失败保留热点与旧配置、失败和取消恢复旧配置、旧 session 提交拒绝、重复提交拒绝、teardown 失败保留所有权、等待 disconnect 后连接。
- `test_szpi_wifi_credentials`：空配置、blob 版本、写失败 / commit 失败、清除失败和成功。NVS / radio 为主机 fault injection mock，不代表真机掉电持久性证明。
- 单 HTML 无外部资源；gzip 构建内容可还原。未烧录、未清除任何实际设备 Wi-Fi / NVS / SD，未修改分区。

主机复现：

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Icomponents/szpi_wifi/src tests/test_szpi_provision_protocol.c components/szpi_wifi/src/provision_protocol.c -o /tmp/test_szpi_provision_protocol
/tmp/test_szpi_provision_protocol
cc -std=gnu11 -Wall -Wextra -Werror -Itests/provision_stubs -Icomponents/szpi_wifi/include tests/test_szpi_wifi_service.c -o /tmp/test_szpi_wifi_service
/tmp/test_szpi_wifi_service
cc -std=gnu11 -Wall -Wextra -Werror -Itests/provision_stubs -Icomponents/szpi_wifi/include -Icomponents/szpi_wifi/src tests/test_szpi_wifi_credentials.c components/szpi_wifi/src/wifi_credentials.c components/szpi_wifi/src/provision_protocol.c -o /tmp/test_szpi_wifi_credentials
/tmp/test_szpi_wifi_credentials
cmake --build simulator/build
cc -std=gnu11 -Isimulator -Isimulator/build/_deps/lvgl-src -Icomponents/szpi_ui/include tests/test_szpi_network_ui.c components/szpi_ui/assets/fonts/szpi_ui_font_button.c simulator/build/_deps/lvgl-build/lib/liblvgl.a -o /tmp/test_szpi_network_ui $(pkg-config --libs sdl2) -lm -lstdc++
/tmp/test_szpi_network_ui
```

系统 GCC 的 ASan / UBSan runtime 链接文件缺失，本轮这些主机测试未在 sanitizer 下执行。测试使用真实生产协议、凭据和状态转换源码。

## 待板上验收

| 场景 | 期望 |
| --- | --- |
| 空 NVS / 重启 | 引导 Network，无自动配网；进入热点 QR 页才启动 WPA2 / DHCP |
| iOS / Android 相机 | 标准 Wi-Fi QR 自动加入 WPA2 热点 |
| Android 支持 DPP 的设置入口 | bootstrap、认证、凭据下发与 DHCP 成功；不支持机型能用 Hotspot |
| DPP / SoftAP 独立启动 | DPP 页无热点，热点页无 DPP，切换前停止旧会话 |
| Android / iOS portal 探测 | DNS / HTTP 弹窗；未弹窗仍能手动访问数字地址 |
| 错误密码、AP 关闭、DHCP 不响应 | 15 秒内失败，热点和网页仍可用，NVS 旧配置不变 |
| 目标 AP 不同信道、隐藏 SSID、长 / 非 ASCII SSID | 手机重连热点后状态可达，连接正确；LCD 缺字限制被明确记录 |
| 目标 LAN 也是 192.168.4.0/24 | 检查 APSTA 路由与 portal 返回，记录冲突行为 |
| 换网测试中取消 / 超时 / 重启 | 旧 blob 和旧网络可恢复，没有永久丢失配置 |
| NVS 空间不足 / 写入时掉电 | 不自动擦 NVS，不误报保存成功，重启恢复最后有效 blob |
| DHCP 成功与退出竞争 | 以保存成功为提交边界，没有部分配置 |
| 错令牌、旧令牌、外部 Origin、STA 侧访问、恶意 JSON | 拒绝危险请求，不打印密码 |
| 扫描中取消、HTTP 慢请求、客户端突断 | 限时停止依赖任务 / socket，后续会话可以重新开始 |
| 成功后的 5 秒 | 手机可读成功状态，之后 AP / DNS / HTTP / DPP 全部停止，STA 保持 |
| 持续重配、camera/audio/UI 同时工作 | 栈、内部堆、队列峰值与 PSRAM 长期稳定，无泄漏 / reset |

NVS Encryption / Flash Encryption / Secure Boot 和量产密钥注入均未执行，需独立安全部署与验收。

## sys_evt 栈溢出修复

用户板上点击配网时确认出现 `A stack overflow in task sys_evt`。原默认事件栈只有 2304B，而 DPP URI 事件和 Wi-Fi 消息在多个回调层按值叠加为大自动对象；主机状态测试不能证明 IDF 默认事件任务栈够用。已把这两个串行回调的大对象改为固定暂存区，不向队列传临时指针；系统事件栈同步至 4096B，新增 sys_evt 最小剩余字节诊断。原有任务 high-water 日志也修正为 IDF 6.1 的字节单位。

`python3 tests/check_wifi_callback_stack.py build/compile_commands.json` 使用真实 Xtensa 编译参数和 `-fstack-usage`：`wifi_event_handler` 64B、`szpi_wifi_post_event` 32B，均通过 <=160B 的自动帧预算检查。静态帧数字不含 IDF / FreeRTOS 完整调用链，不能替代板上剩余栈观测。主机还验证暂存区清零后 queue 值仍保留完整 URI。

修复后的重新烧录、反复点击配网和最小剩余栈检查仍待用户上板复测。未由本次开发操作烧录或擦除设备。

最终验证：栈修复后的当前配置 `idf.py build` 与全新 `/tmp/szpi-provision-stack-clean` defaults 构建均通过；主机协议 / NVS / service / UI 测试及 GCC callback 栈预算检查通过。新版当前固件产物为 `build/esp32-szpi.bin`。板上复测状态仍为待完成。

## Setup unavailable 修复

用户板上反馈进入配网显示 Setup unavailable。检查本地 IDF 6.1 的 esp_netif_set_ip_info 实现确认：AP 默认 netif 的 DHCP 状态为 INIT 时也不能修改 IP，必须显式 STOPPED。旧启动代码省略 DHCP stop，因而会返回 ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED。已按停止 DHCP、设置 AP IP/DNS/DNS option、恢复 DHCP 自动启动、配置有密码的 AP、启动 radio、限时等待 AP netif up、确认 DHCP、启动 portal 的顺序修正。DNS socket 在 netif 就绪后才绑定。

新增 test_szpi_wifi_ap_netif 使用生产配置函数验证 DHCP INIT / STARTED / STOPPED 三种初始状态、设置顺序、DNS offer 类型和值、每一步失败停止后续操作并保留失败阶段。测试通过；固件重新构建通过。启动错误现在保留具体阶段与 esp_err 名称，portal 错误额外记录 socket errno，不输出密码。板上重新进入配网仍待复测。

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Itests/provision_stubs -Icomponents/szpi_wifi/src tests/test_szpi_wifi_ap_netif.c components/szpi_wifi/src/wifi_ap_netif.c -o /tmp/test_szpi_wifi_ap_netif
/tmp/test_szpi_wifi_ap_netif
```

## 独立页面与按需启动

按用户要求，DPP 与热点使用独立二维码页，仅在页面加载完成后启动各自会话；启动无配置、清除配置、进入方式选择页均不启动配网。右划卸载页面停止会话，取消按钮移除。DPP QR 196px，SoftAP QR 192px，SSID / 密码 / 手动访问地址在热点页右侧显示。

固件及 simulator 构建通过；状态机主机测试覆盖无配置不启动、DPP / SoftAP 方式选择与旧会话隔离；UI 测试覆盖独立页面、加载后启动、快速退出请求 ID、无按钮、二维码尺寸与反复创建销毁，渲染检查通过。板上手机扫描、获取 DHCP 地址与 DPP 认证仍待复测。

## 手机复测与 HTTP 双栈修复

用户确认 DPP 实际成功，先前 reason=201 对应未发现目标 AP；设备仅支持 2.4GHz。STA 显式使用全信道扫描、不固定 BSSID，候选失败 reason=201 时显示网络未找到与 2.4GHz 提示，保留旧配置。

SoftAP 访问被 Request rejected 的原因：本项目开启 LWIP IPv6，IDF HTTP server 使用 PF_INET6 双栈 socket；旧校验把 getsockname / getpeername 地址读成 sockaddr_in，误拒 IPv4-mapped IPv6 客户端。现使用 sockaddr_storage，校验地址族和长度，将 IPv4-mapped IPv6 转换成 IPv4，仍要求本地 192.168.4.1 和有效热点客户端地址；原生 IPv6 / 非热点来源继续拒绝。test_szpi_portal_address 覆盖 IPv4、mapped IPv6、原生 IPv6 和未知地址族，测试通过；Wi-Fi 状态测试覆盖 reason=201 不保存及提示，固件构建通过。

http_parser 错误 16 在本地 IDF 对应 INVALID_METHOD，只凭日志无法确定请求来源；可能是 HTTPS / 非 HTTP 请求发到 80 端口，不能通过跳过 HTTP 解析或劫持 HTTPS 处理。手机需使用完整 http://192.168.4.1/；本次网页修复与自动弹窗仍待上板复测。未执行烧录。

## 配网成功反馈与扫描提示

用户上板确认 DPP 和 SoftAP 配网均成功。网页扫描期间显示 Scanning nearby networks，扫描按钮禁用并显示 Scanning；完成后状态区域持续显示 Scan complete 与列表中网络数量。空结果提示 No networks found，失败提示重试或手动输入。扫描数对应去重后页面列表，不宣称枚举全部附近 AP。固件构建与现有 Wi-Fi 状态机回归测试通过；扫描提示待手机复测。

## Current Wi-Fi 连接详情

新增实际 SSID / 状态 / IP / 网关 / 子网 / 设备 MAC / AP MAC / RSSI / 信道。共享详情区域可上下滚动，清除入口保留二次确认。断线不展示上次 IP / AP MAC / 信号；MAC 来自 STA 地址，模拟器数据为演示值。固件和 simulator 构建通过；共享页面渲染与断线清空、滚动内容测试通过。RSSI 随现有 UI 刷新更新，板上实时数值和触摸滚动待复测。

## 启动状态查询警告

用户启动日志出现密集 Haven't to connect to a suitable AP now，源于 UI 高频刷新调用新增连接详情查询，在未关联时反复调用 esp_wifi_sta_get_ap_info。适配层使用原子关联标志，CONNECTED 设置、DISCONNECTED / STOP 清除；RSSI 与详情在未关联时直接返回未连接 / 无链路快照，不调用 AP 查询，仍读取设备 MAC。成功停止驱动也清除标志。未屏蔽驱动日志，真正连接失败事件仍保留。固件构建、Wi-Fi 状态机回归和回调栈预算检查通过，启动日志待上板复测。

## 首次联网校时

用户反馈初次连接后时间未自动更新。UI 原本持续刷新，但 SNTP 原逻辑只初始化一次、后续 GOT_IP 不重启同步，初次失败缺少及时重试与日志。现在配网提交成功后等待配网资源关闭再发起同步，正常 STA GOT_IP 发起 / 重启同步；首次未同步时每 30 秒最多额外重启 3 次，之后保留 SNTP 常规后台轮询。请求、同步成功与未同步均记录日志；UI 在同步回调设置 TIME_SYNCED 后自动显示 CST 时间，无需重启。互联网 DNS / UDP 123 可达性仍是前提，DHCP 成功不代表时间服务器可达。主机状态测试覆盖配网成功关闭前不启动、关闭后启动及重新请求重启。固件构建和主机状态测试通过，上板首次校时待复测。

2026-10-10：SoftAP 网页扫描提示位于扫描按钮下方。网络列表标为 Choose a network，初始无选中项且隐藏连接表单；选择扫描 SSID 后显示表单并锁定 SSID，选择 Manually Enter 后允许编辑 SSID。切换网络清空密码；扫描列表刷新保留仍有效的选中项，已移除的网络取消选择并隐藏表单。手动输入时才显示 WPA3 only 选项。
