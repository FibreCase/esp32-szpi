# 第二阶段验证记录

日期：2026-10-07。状态：**实现及构建通过；除坐标映射主机测试外，其余逻辑测试和上板验收未完成。**

## 依赖与实现

- ESP-IDF：6.1；目标：ESP32-S3；保持 MINIMAL_BUILD、16MiB Flash、Octal 80MHz PSRAM 与双 OTA 分区。
- LVGL：`lvgl/lvgl 9.6.0~1`，由 `components/szpi_display/idf_component.yml` 固定，`dependencies.lock` 由组件管理器生成。2026-10-07 查询时该版本为组件仓库最新稳定包；参见 [LVGL 9.6.0~1](https://components.espressif.com/components/lvgl/lvgl/versions/9.6.0~1/readme)。
- LCD：ESP-IDF `esp_lcd` ST7789 驱动，SPI2 / 20MHz / mode 2 / RGB565；方向、反色及字节序仍待实物确认。
- 触摸：FT6336 地址 `0x38`，使用共享 I²C master bus 和新 I²C API；官方驱动所标型号为 FT6336U，未将型号相近当作兼容性证据，因此实现本地有界读取。
- 触摸初始映射是假设值：`x=319-raw_y, y=raw_x`，预期原始坐标范围 240×320；四角实际坐标尚未测量。
- 两个内部 DMA draw buffer 合计 25,600 bytes；`szpi_ui` 静态栈 6,144 bytes。

资源归属与释放顺序：

| 资源 | 所有者 | 容量 / 数量 | 销毁者及顺序 |
| --- | --- | --- | --- |
| SPI2 总线 | `szpi_display` | 单总线，最大传输 25,600 bytes | `szpi_display_deinit`；等 DMA 完成后删除 panel/io、释放总线，再释放 PCA9557 LCD_CS |
| panel IO / ST7789 panel | `szpi_display` | 各 1 个 | 先删除 panel，再删除 panel IO，随后释放 SPI 总线 |
| LEDC timer1/channel1 | `szpi_display` | 各 1 个 | UI stop 先设亮度 0，deinit 停止 channel 并 deconfigure timer |
| LVGL display / 两块 DMA buffer | `szpi_display` | display 1 个；buffer 各 12,800 bytes | UI 删除 indev 后，display 组件删除 LVGL display 并释放两块 buffer |
| DMA completion semaphore | `szpi_display` | 静态计数 semaphore，容量 2 | 静态生命周期，不动态销毁；每次初始化先清空旧计数 |
| FT6336 I²C 设备句柄 | `szpi_input` | 1 个，board 总线借用 | UI stop 在显示资源释放前删除设备句柄；不删除共享总线 |
| UI stack / status lock | `szpi_app` runtime | 静态栈 6,144 bytes；静态 mutex 1 个 | runtime 拥有静态生命周期 |

## 构建与配置

- 主构建命令：`idf.py build`（ESP-IDF 6.1 环境）。结果：成功。
- 主构建应用镜像：`0x120a30` bytes；最小 OTA 应用槽：`0x7f0000` bytes；剩余 `0x6cf5d0` bytes（约 86%）。分区生成表中 `ota_0` / `ota_1` 各为 8128KiB。
- 独立 defaults 完整构建：`idf.py -B /tmp/szpi-phase2-defaults -D SDKCONFIG=/tmp/szpi-phase2-defaults/sdkconfig build`；配置目标为 ESP32-S3，Flash 16MiB、Octal PSRAM 80MHz，`LV_BUILD_EXAMPLES` / `LV_BUILD_DEMOS` 均关闭，Wi-Fi SSID / password 为空。构建成功，镜像 `0x120a10` bytes，OTA 余量 `0x6cf5f0` bytes（约 86%）。
- 当前生成的项目 `sdkconfig` 可能含本机 Wi-Fi 凭据；未写入本文或版本控制。
- 构建输出有 ESP-IDF 配置提示：`ESP_INT_WDT_TIMEOUT_MS` 当前值为 300ms，与 IDF Kconfig 默认值 800ms 不同，构建仍成功。该设置来自此前项目配置，本阶段未改动。

## 已执行的逻辑检查

- `cc -std=c11 -Wall -Wextra -Werror tests/test_szpi_input_transform.c -o /tmp/test_szpi_input_transform && /tmp/test_szpi_input_transform`：通过。覆盖四角、中心、原始坐标越界、空输出指针，以及双触点时保持主 ID、主触点释放后切换和全部释放状态。
- 尚未覆盖触摸寄存器读取失败 / RELEASED、稳定主触点选择、刷新区域边界、提交失败、DMA 完成与超时缓冲所有权、初始化失败逆序清理、服务重复启停和触摸降级。

## 尚未进行的上板验收

- 未烧录本阶段固件；没有颜色、旋转、反色、最后行列可见性或触摸坐标的实测结果。
- 未测背光 10% / 50% / 100%、启动及停止背光电平。
- 未测 Wi-Fi 状态展示、100 次点按、拖动、10 次启停、堆变化或连续运行 30 分钟。
- 坐标、颜色顺序、SPI 字节序及方向初值仍需按 `docs/hardware_io.md` 的说明逐项校准。
