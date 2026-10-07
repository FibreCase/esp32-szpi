# esp32-szpi

基于嘉立创实战派 ESP32-S3 开源硬件的 ESP-IDF 项目。第一阶段已实现正式启动诊断、共享 I²C/PCA9557 安全状态、静态 FreeRTOS runtime 与有界 Wi-Fi STA 服务；逻辑测试和上板验收仍待完成。

## 硬件与配置

开发板采用 ESP32-S3-WROOM-1-N16R8 模组：

- Flash：16MiB，Quad SPI（QIO），80MHz STR。
- PSRAM：8MiB，Octal SPI，80MHz；启动时自动检测、初始化并执行内存测试，加入 `malloc()` / `heap_caps_malloc()` 可用堆。
- ESP-IDF：当前开发环境为 6.1，目标为 `esp32s3`。

持久化配置见 `sdkconfig.defaults`，当前生效配置为生成的 `sdkconfig`。ESP-IDF 对已有 `sdkconfig` 的值优先处理，修改 defaults 后需要同步已有配置；全新项目会自动加载 defaults。PSRAM 容量由硬件检测，不能通过配置扩大；预期检测到 8MiB。

## 自定义分区表

`partitions.csv` 参考 ESP-IDF 6.1 的 `components/partition_table/partitions_two_ota_large.csv`（双 OTA、无 factory），保留其 NVS、OTA 元数据和 PHY 分区，将两个 OTA 槽分别扩大到 8128KiB（7.9375MiB），占满首个应用偏移之后的全部 Flash 空间。分区表位于 `0x8000`；CSV 留空偏移，由官方工具自动对齐。

| 分区 | 类型 / 子类型 | 自动生成偏移 | 大小 |
| --- | --- | --- | --- |
| nvs | data / nvs | 0x9000 | 24KiB |
| otadata | data / ota | 0xF000 | 8KiB |
| phy_init | data / phy | 0x11000 | 4KiB |
| ota_0 | app / ota_0 | 0x20000 | 8128KiB |
| ota_1 | app / ota_1 | 0x810000 | 8128KiB |

`ota_1` 结束于 `0x1000000`（16MiB 边界），没有尾部预留空间。没有 factory 分区，OTA 元数据为空时启动 `ota_0`。这里只配置 OTA 所需布局，后续仍需实现固件下载、校验及切换逻辑。

## 硬件接线速查

[GPIO 与外设连接](docs/hardware_io.md) 结合原理图 V1.0.1 与硬件说明图，整理了 ST7789 320×240 显示屏、FT6336 触摸等型号参数，以及模组引脚、共享 I²C 地址、PCA9557 控制信号及 LCD、摄像头、音频、SDMMC 接线，可作为板级编码参考；文档已附基于官方例程和器件资料选定的初始驱动配置。

## 代码结构与开发规则

[代码结构规划](docs/code_architecture.md) 定义启动装配、应用服务、外设适配、板级资源的边界，以及 FreeRTOS 统一任务启停、资源预算、共享资源、缓冲生命周期和分阶段验收规则。后续编码遵守根目录 [AGENTS.md](AGENTS.md)。

## 第一阶段目标

[task_phase_1.md](docs/develop/task_phase_1.md) 跟踪基础资源、安全启动、FreeRTOS runtime 和 Wi-Fi STA 的实现与验收。当前构建与配置核对通过，已记录单次正常联网启动；逻辑测试、安全电平及完整上板验收仍待完成，详见 [验证记录](docs/phase_1_validation.md)。

Wi-Fi 开发期设置通过 `idf.py menuconfig` 中的 **SZPI application** 项填写。默认 SSID / 密码为空；填写后的值会编入本地生成的 `sdkconfig` 与固件，不应提交或分享凭据。

## 第二阶段目标

[task_phase_2.md](docs/develop/task_phase_2.md) 跟踪 ST7789 显示、FT6336 触摸、LVGL 和单屏验证 UI。UI 任务由 FreeRTOS runtime 统一创建，页面实现颜色 / 方向标记、点击计数、亮度滑条及触摸状态；固件构建和干净默认配置核对通过。坐标映射、刷新边界等逻辑测试及实物显示 / 触摸校准仍待完成，详见 [验证记录](docs/develop/phase_2_validation.md)。

## 构建与烧录

在已激活 ESP-IDF 6.1 的终端执行：

```sh
idf.py build
idf.py partition-table
idf.py -p PORT flash monitor
```

将 `PORT` 替换为实际串口。首次使用此布局，应完整烧录 bootloader、分区表和应用（`idf.py flash`）。硬件验证时，检查启动日志是否识别 8MiB PSRAM、内存测试通过，以及 Flash 配置为 16MB。

## 参考

- [嘉立创开发板介绍](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/introduction.html)
- [乐鑫模组数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP-IDF Flash 与 PSRAM 配置](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/flash_psram_config.html)
- [ESP-IDF OTA 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/ota.html)

LCD 空白屏排查可在 `menuconfig → SZPI display` 覆盖 SPI 频率或模式，记录见 [第二阶段验证](docs/develop/phase_2_validation.md)。

上板进展：用户已确认官方 reset / CS 顺序修正后屏幕可显示；触摸按反馈修正 180° 旋转，待复测。

用户已确认显示和触摸效果正常；启动彩条诊断已移除，启动直接进入验证 UI。

当前高刷新目标：LCD SPI 80MHz、LVGL 16ms 刷新周期，双内部 DMA 缓冲各 40 行；配置与构建不代表实测全屏 60 FPS，高速稳定性及帧率待上板验证。
