# 第一阶段：基础资源、启动与 Wi-Fi 连接

制定日期：2026-10-07。状态：**实现、构建及单次联网启动验证完成；逻辑测试与完整上板验收待完成**。本文记录第一阶段实现任务与验收进度；阶段整体尚未完成。

依据：[代码结构与规则](docs/code_architecture.md)、[硬件接线与初始参数](docs/hardware_io.md)、[AGENTS.md](AGENTS.md)。第一阶段引入 Wi-Fi 能力，属于当前明确需求；后续产品业务仍按需规划。

## 1. 阶段目标

将现有 Hello World 改为能够持续运行的正式启动程序，完成：

1. 验证 ESP32-S3 / 16MiB Flash / 8MiB PSRAM 与双 OTA 布局，建立内存和启动诊断。
2. 初始化共享 I²C 与 PCA9557，设置确定的安全输出状态。
3. 建立 FreeRTOS 统一任务入口、资源归属和系统状态，支持故障上报及协作停止。
4. 连接指定的 2.4GHz Wi-Fi AP，获取 DHCP IPv4 地址，提供可查询的网络状态与有限重连。
5. 用构建、状态机测试与上板记录证明成功路径和失败路径均可控。

**完成标志：** 配置有效 AP 后获得 IPv4，系统保持运行；未配置、密码错误、AP 不可达或断线时没有重启循环、无限等待或资源泄漏，且状态与原因可观察。

## 2. 范围

本阶段实现 board、runtime、Wi-Fi 和最小启动配置。屏幕、触摸、IMU、摄像头、音频、SD 文件系统、BLE、SoftAP / 配网、HTTP / MQTT、互联网连通检测和实际 OTA 升级留到后续阶段。

仅设置 LCD CS / 背光、功放、摄像头的安全状态，不启动对应外设。双 OTA 表继续保留两个等大 8128KiB 槽，无 factory，不新增分区。Wi-Fi 获取 IP 不等于已验证互联网可用。

## 3. 代码落点与边界

| 位置 | 第一阶段职责 |
| --- | --- |
| main/app_main.c | 调用正式启动入口，移除示例倒计时重启，保持入口简洁 |
| main/app_boot.c | 初始化基础条件、装配 board / runtime / Wi-Fi，汇总启动结果 |
| main/Kconfig.projbuild | 项目级 Wi-Fi 开关与开发期 SSID / 密码输入 |
| components/szpi_board | 引脚绑定、I²C 总线、PCA9557 及语义控制接口 |
| components/szpi_app | runtime、任务 / 资源表、supervisor、Wi-Fi 连接策略服务 |
| components/szpi_wifi | ESP-IDF Wi-Fi / STA netif / 事件适配、状态快照与生命周期 |

依赖为 `main -> szpi_app -> szpi_wifi / szpi_board -> ESP-IDF`。Wi-Fi 协议能力不依赖 board 引脚，也不调用 supervisor；通过短事件通知把结果交给服务。重试时序、系统降级属于应用服务，驱动适配只执行命令并返回状态。

`szpi_app/src/wifi_service.c` 提供任务入口，由 runtime 创建；`szpi_wifi` 不自行创建项目任务。网络底层使用 ESP-IDF 内置任务，登记为依赖任务。当前不创建其他阶段的空组件。

## 4. 启动流程

```text
app_main
  -> 记录固件 / IDF 版本、复位原因与存储信息
  -> 校验 Flash / PSRAM / 分区
  -> 初始化 NVS
  -> 初始化 board I²C 与 PCA9557，建立安全状态
  -> 创建 runtime 队列 / 状态对象 / supervisor
  -> 发布 SYSTEM_READY（只表示基础资源就绪）
  -> Wi-Fi 禁用或 SSID 为空：发布 DISABLED / NO_CONFIG
  -> 否则初始化 ESP-NETIF、默认事件循环、STA 与 Wi-Fi 服务
  -> 异步连接 AP，获得 IPv4 后发布 NETWORK_READY
```

- SYSTEM_READY 与 NETWORK_READY 是独立状态，基础就绪不等待网络；不能在 app_main 或 supervisor 内永久等待连接。
- 核心内存、NVS 或安全输出初始化失败，进入系统 FAULT，禁止继续开启外设；能运行诊断任务时保留诊断。runtime 本身创建失败则明确报告并停止后续启动。
- Wi-Fi 初始化 / 连接失败是网络能力故障，系统仍保持基础诊断，不重启整机。
- NVS 不自动整区擦除。`NO_FREE_PAGES` / `NEW_VERSION_FOUND` 等情况报告具体错误并进入恢复待处理状态，保留数据；擦除不属于默认修复流程。
- 停止 Wi-Fi 不删除系统拥有的默认事件循环，也不重复初始化整个 TCP/IP 栈；Wi-Fi 的 STA netif、handler、驱动实例由自身生命周期管理。

ESP-NETIF 的系统初始化与 STA 生命周期顺序参照 [ESP-IDF ESP-NETIF 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_netif.html)，TCP/IP 初始化和默认事件循环各只创建一次。

## 5. 基础资源任务

### P1-01：启动和内存诊断

- [x] 移除 Hello World 倒计时和无条件 esp_restart。
- [x] 输出目标芯片、固件 / IDF 版本、复位原因、Flash 实际容量、PSRAM 初始化结果及容量。
- [x] 用分区 API 校验 ota_0 / ota_1 类型、等大尺寸及 Flash 边界，记录运行分区；不硬编码 OTA 写入目标。
- [x] 确认 PSRAM 为 8MiB；启动时执行有界小块分配 / 读写 / 释放检查，不申请整块 8MiB。
- [x] 记录内部 RAM / PSRAM 的当前与最低剩余堆；统计按同一种口径比较，不把总堆当作内部 RAM。
- [x] 对必需检查失败提供明确错误，避免“检查失败但继续报告成功”。

### P1-02：board 与安全状态

- [x] 固定 GPIO / I²C 地址 / 扩展器 bit 集中在 board；实现只读 bindings 和语义控制接口。
- [x] 使用 IDF 新 I²C master 接口，一次创建 I2C_NUM_0：SDA=1、SCL=2、100kHz，事务有超时。
- [x] 接入 PCA9557 0x19；先写 Output=0x05，再写 Polarity=0x00、Configuration=0xF8。
- [ ] 验证安全状态：LCD CS 释放、功放关闭、GC2145 PWDN 高；GPIO42 拉高关闭背光，本阶段不创建背光 PWM。
- [x] 不触碰 PSRAM GPIO35/36/37，不重新配置共用 RESET，不占用 GPIO10/11 或其他未启用外设接口。
- [x] 扩展器更新由 mutex 保护，写成功再更新影子值；读回配置与输出锁存寄存器辅助验证，不能仅凭日志宣称实际输出有效。
- [x] 单次上板启动日志确认 PCA9557 输出 / 极性 / 方向读回为 `0x05 / 0x00 / 0xF8`；物理引脚及各外设安全状态仍待电气验证。
- [x] init 失败逆序清理设备 / 总线 / 锁；重复 init 返回 INVALID_STATE。

不要求全总线扫描作为正常启动动作；可按需诊断已知地址。摄像头在 PWDN 状态下不应被列为本阶段必须应答的设备。

### P1-03：FreeRTOS runtime

- [x] 统一任务表、资源登记、错误消息和能力状态。
- [x] supervisor：优先级 3，栈初始 4096 字节；Wi-Fi 服务：优先级 4，栈初始 4096 字节；默认 tskNO_AFFINITY。
- [x] 项目长期任务与必要队列 / 事件组静态创建，任务栈放内部 RAM；任务启动屏障与创建失败清理避免留下半启动服务。
- [x] 使用有界消息队列；supervisor 与 Wi-Fi 消息队列各 16 项，消息为小结构体且无借用事件指针。
- [x] 定义 SYSTEM_READY、NETWORK_READY、SYSTEM_FAULT 等状态 bit，状态与任务调度状态分开。
- [x] supervisor 以事件等待为主，每 10 秒采集堆 / 栈余量 / 队列峰值 / 失败计数；不每轮刷屏。
- [x] 任务不忙等、不直接 return；协作 STOP 能打断重试等待并确认停止，停止前释放在途资源。
- [x] 保留 IDF watchdog 基线；本阶段未增加 task/user 订阅，不由 supervisor 替其他任务喂狗。

## 6. Wi-Fi 功能任务

### P1-04：配置入口

- [x] Kconfig 项目开关 `CONFIG_SZPI_WIFI_ENABLED` 默认启用；SSID / 密码默认空，通过本地 menuconfig 填入。
- [x] 接收有界配置结构，按 ESP-IDF 字段容量和 WPA2-PSK 格式验证长度；支持合法 32 字节 SSID，避免截断或越界。
- [x] 配置为空进入 NO_CONFIG，基础启动照常完成；不扫描重试空 SSID。
- [x] 第一阶段只实现指定 AP 的 STA、DHCP IPv4；默认要求 WPA2-PSK 及以上，不自动降低到开放 AP / WEP。
- [x] 使用 WIFI_STORAGE_RAM 传入本轮凭据，NVS 仍作为系统 / Wi-Fi 初始化基础保留。
- [x] SSID / 密码不硬编码在 .c 或跟踪的 defaults 中；实际值只保留在本地生成 sdkconfig，密码不写日志。menuconfig 凭据会编入固件，运行时配网与独立持久化接口后续实现。

### P1-05：事件驱动连接

- [x] 在全局网络初始化后，只创建一个默认 STA netif，注册可撤销的事件 handler，配置并启动 Wi-Fi。
- [x] 处理 STA_START、STA_CONNECTED、STA_DISCONNECTED、STA_GOT_IP、STA_LOST_IP / STA_STOP 等相关事件。
- [x] STA_CONNECTED 只说明 AP 链路建立；STA_GOT_IP 才发布 NETWORK_READY。
- [x] 断线撤销 NETWORK_READY 与旧 IP 快照，不等待延后的 LOST_IP 通知。
- [x] 回调只复制必要字段并投递消息；不延时、不重连循环、不阻塞等待队列、不操作 board I²C。
- [x] 事件消息不保存 event_data 指针；队列满时计数并通知服务保守降级 / 状态核对。
- [x] Wi-Fi 状态快照包含状态、IPv4 / 网关、RSSI（在线按需读取）、最近 disconnect reason、尝试次数及最后错误；读取具备线程安全性。

初始化与事件行为参照 [ESP-IDF 6.1 STA 官方例程](https://github.com/espressif/esp-idf/blob/v6.1/examples/wifi/getting_started/station/main/station_example_main.c)和 [Wi-Fi 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/index.html)。采用其 API 流程，不沿用例程的密码日志、自动擦 NVS 或永久阻塞等待做法。

### P1-06：有限重试与主动停止

| 参数 | 第一阶段初始值 |
| --- | --- |
| 连接模式 | STA，2.4GHz，DHCP IPv4 |
| 单次连接 / DHCP 等待上限 | 15 秒 |
| 一轮最大尝试次数 | 5 次，包含首次 |
| 重试间隔 | 1、2、4、8 秒 |
| 整轮总截止时间 | 90 秒，使用单调时钟；达到任一限制即结束 |
| 连接成功 | 清零本轮失败计数，记录 GOT_IP |
| 运行期间断线 | 开启一轮同样有界的恢复尝试 |
| 耗尽预算 | FAILED，保持系统诊断；收到明确 RETRY / 新配置后才开启新一轮 |
| 主动 STOP | 取消重试、清除 READY、停止 Wi-Fi；不触发断线自动重连 |

- [x] 状态区分 DISABLED、NO_CONFIG、STOPPED、CONNECTING、LINK_UP、ONLINE、RETRY_WAIT、FAILED、STOPPING。
- [x] 连接策略仅由 Wi-Fi 服务执行，event handler 不主动连接。
- [x] 超时包含获取 IP；超时后结束本次连接，之后进行有限重试。
- [x] 明确 STOP / RETRY 命令与网络事件的处理；STOP 后迟到事件不会重新标记 ONLINE。
- [x] 本轮耗尽后不会无限自动开新轮；窗口内持续尝试，窗口结束后需显式 RETRY。
- [x] 实现可调用的 start / stop / retry / get_status 接口，不额外创建串口命令解释器。
- [x] stop 等待上限为 3 秒；事件 handler 与回调上下文在服务生命周期内保持有效，不反复创建服务任务。

上表是工程初始策略，非 Wi-Fi 协议标准。低层事件异常或资源失败可直接进入 FAILED；耗尽重试仍需保留可诊断的失败原因。

## 7. 实现顺序与交付物

| 顺序 | 任务 | 交付 / 检查点 |
| --- | --- | --- |
| 1 | P1-01 | 正式 boot 诊断，取消示例重启，存储配置核对 |
| 2 | P1-02 | board 安全输出、I²C / PCA9557 清理与测试 |
| 3 | P1-03 | runtime、两个项目任务、资源表及故障状态 |
| 4 | P1-04 / P1-05 | Wi-Fi 配置、STA 生命周期、事件到状态转换 |
| 5 | P1-06 | 重试期限、协作 STOP、失败 / 恢复流程 |
| 6 | 验收 | 构建及逻辑测试结果、上板用例记录、更新文档 |

Wi-Fi 使用 ESP-IDF 内置组件，本阶段优先不增加外部依赖。每个实际组件明确 CMake 依赖，保留 MINIMAL_BUILD ON 和 esp_psram；完成时记录新增配置的生效值与内存预算。验证记录存为 `docs/phase_1_validation.md`，实际执行后再创建，结果不能预填“通过”。

## 8. 验收清单

### 构建与可复现性

- [x] ESP-IDF 6.1 `idf.py build` 通过；本组件无编译警告或未知配置符号。构建环境另有 IDF 组件校验及既有 WDT 默认值差异提示，详见验证记录。
- [x] 在独立构建目录使用 defaults 生成配置验证 Flash=16MiB、Octal PSRAM、双 OTA / 无 factory；Wi-Fi 凭据为空且不进入 defaults。
- [x] 分区生成工具校验通过，ota_0 / ota_1 各 0x7F0000，尾部到 16MiB 边界。
- [x] 依赖与头文件边界符合规则，所有项目任务的创建都经过 runtime。

### 必要逻辑测试

- [ ] PCA9557 更新一位保留其他位；写失败不更新影子值；初始化中途失败正确清理。
- [ ] Wi-Fi 只在 GOT_IP 后 READY；DISCONNECTED 清除 READY；STOP 后迟到事件不能重新上线。
- [ ] 单次 / 总超时、5 次尝试与退避符合策略；FAILED 不自行开启无限新轮。
- [ ] 队列满、无配置、重复 start、停止重试中的服务有明确处理；计时逻辑测试使用可控时钟，不真的等待 90 秒。

### 上板用例

| 已执行记录 | 结果 |
| --- | --- |
| 2026-10-07 冷启动，16MiB Flash / 8MiB PSRAM，WPA2 STA 与 DHCP | 通过：从 `ota_0` 启动；Flash、PSRAM 容量及 PSRAM memory test 正常；PCA9557 寄存器读回符合配置；连接成功并取得 IPv4 `192.168.31.180`；运行日志至少持续到约 20 秒并输出 supervisor 堆 / 栈统计。SSID 未写入记录。 |

此记录证明一次正常联网启动路径，不代表其他上板用例均已通过。

| 用例 | 预期结果 |
| --- | --- |
| 冷启动，Wi-Fi 禁用 / 未配置 | 基础就绪，无网络连接循环；功放 / 摄像头 / 背光处于关闭状态 |
| 有效 WPA2 AP、正常信号与 DHCP | 目标 30 秒内获得 IPv4，输出网络状态；目标时长是验收环境要求，不保证任意网络 |
| 同局域网检查 | AP 允许 ICMP 时，电脑可 ping 设备 IPv4；不据此宣称互联网可用 |
| 密码错误 / AP 不存在 | 90 秒内或更早进入 FAILED，原因与次数可查询，系统不重启 |
| AP 恢复，仍在重试窗口内 | 自动重连，获得 IP 后重新 READY |
| AP 在窗口耗尽后恢复 | 先保持 FAILED；调用 RETRY 后连接成功 |
| AP 已连接但 DHCP 不分配地址 | 不标记 READY，按超时 / 重试策略退出 |
| 连接中 / 退避中 / 在线时主动停止 | 3 秒内确认停止、取消后续重连，保留基础运行 |
| 10 次 start / stop 循环 | 无重复 STA / handler / task，无持续堆泄漏；记录首次预热后堆变化与任务数量 |
| 连续运行 30 分钟 | 无异常重启 / watchdog / 堆损坏；诊断任务可响应，记录最低堆和栈余量 |

上板确认包括输出寄存器 / 引脚或外设实际状态；只打印“安全状态已设定”不算电气验证。硬件操作按用户授权执行，暂未执行的项目明确标为“未验证”。

## 9. 阶段完成条件

基础资源和启动是必需能力；Wi-Fi 的成功连接路径必须在有效 AP 上通过，失败 / 停止路径也必须符合有界策略。核心实现、逻辑验证和上板验收完成后才将本阶段标为完成；没有设备时可标“实现与构建完成，待上板”，不能直接判定阶段完成。

收尾同步 README、AGENTS 的实际实现状态，以及 hardware_io 中上板修正的参数。后续阶段以这些基础资源和网络状态接口为入口，不重新安装共享资源或绕过 runtime 创建任务。
