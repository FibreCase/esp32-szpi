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

## 第三阶段：camera 与预览

[task_phase_3.md](docs/develop/task_phase_3.md) 跟踪 GC2145 适配、QVGA RGB565 采集及 LVGL 预览；代码已实现 board camera bindings、相机组件、runtime 预览服务及可进入 / 退出的预览页。依赖构建与实物验收状态见[验证记录](docs/develop/phase_3_validation.md)。

## 第二阶段附加任务

[QMI8658A 与 BOOT 按键](docs/develop/task_phase_2_extra.md) 已实现六轴采集、静态倾角、按键消抖及输入验证页，复用 board I²C 并由唯一 UI 任务轮询；构建和主机逻辑测试通过，实物验证见[记录](docs/develop/phase_2_extra_validation.md)。

## 第四阶段任务

[audio / storage 与 FAT32 SD 卡](docs/develop/task_phase_4.md) 已有初版实现：ES7210 双麦声级检查、麦克风增益与扬声器音量滑条、ES8311 测试音、PCM WAV 录卡 / 最近文件回放、SDMMC 1-bit FAT32 挂载及双步显式格式化确认。AUDIO 与 SD CARD 验证入口位于独立页面。IDF 6.1 构建通过；分块录音过载恢复、逻辑测试、并行满负载及实物验收仍待完成，详见[第四阶段验证记录](docs/develop/phase_4_validation.md)。启动挂载失败不会自动格式化，开发验证未格式化实卡。

## 配置兼容性说明

LVGL 9.5 的断言、颜色混合舍入、换行字符与主题选项已在 `sdkconfig.defaults` 显式配置；样式缓存保留开启，以维持当前 UI 刷新性能。已有 `sdkconfig` 也需同步这些值。

本机 ESP-IDF 6.1 的 `components/fatfs/Kconfig` 中，`FATFS_PRINT_LLI` 与 `FATFS_PRINT_FLOAT` 的 bool 默认值为非法的 `0`；已修正为 `n`，行为不变。此修正在项目仓库之外，重新安装 SDK 后若再次出现相同警告，需核对这两处默认值。

## 第五阶段：共享 UI 与 Linux 模拟器

共享页面位于 `components/szpi_ui`，当前使用纯黑背景，顶部状态栏左侧显示 Wi-Fi 状态、中央标题为 `SZ-PI`、右侧显示 NTP 同步后的中国标准时间。主页左划以整页滑动动画打开可纵向滚动的设置菜单；点击菜单项进入位于父菜单右侧的详情页，展示信息行、开关和滑条等本地测试控件，不连接设备服务。详情页右划返回父菜单，设置页右划回主页，页面切换均带滑动动画且不显示返回按钮。`szpi_app` 负责板上 LVGL 任务、SNTP 校时、触摸输入、IMU 轮询、依据 X 轴加速度自动 180° 旋转和请求桥接，UI 组件不依赖硬件。Linux 模拟器和固件共用 UI 源文件清单，LVGL 固定为 9.5.0，SDL2 使用系统开发包。构建及运行方式和验证边界见[第五阶段记录](docs/develop/phase_5_validation.md)：

设置菜单及其详情页顶部左侧显示 `< 上一级标题`（Home、Settings 或 Audio），中间显示当前标题。滑条卡片显示不可拖动的占比预览；点击后进入禁用划页手势的独立调节页，使用大滑条调整演示值，并通过 Back 按钮返回。

```sh
cmake -S simulator -B simulator/build
cmake --build simulator/build
./simulator/build/szpi_ui_sim
```

Fedora 安装 SDL2 开发包：`sudo dnf install SDL2-devel pkgconf-pkg-config`。模拟器窗口固定为 320×240、1:1 显示且不可调整大小；鼠标拖动可模拟触摸划页，键盘右方向键进入设置、左方向键返回，B 模拟 BOOT 短按，Esc 退出。UI 使用英文和 Noto Sans，字体以仅含 ASCII 字符的 C 源码嵌入固件与模拟器；生成方式与许可证记录在[第五阶段验证记录](docs/develop/phase_5_validation.md)。模拟器使用主机本地时间；固件使用 SNTP 校时。
