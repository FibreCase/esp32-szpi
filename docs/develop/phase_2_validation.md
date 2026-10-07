# 第二阶段验证记录

日期：2026-10-07。状态：**实现及构建通过；坐标映射主机测试通过，用户确认显示及触摸正常；其余逻辑测试和量化上板验收未完成。**

## 依赖与实现

- ESP-IDF：6.1；目标：ESP32-S3；保持 MINIMAL_BUILD、16MiB Flash、Octal 80MHz PSRAM 与双 OTA 分区。
- LVGL：当前固定 `lvgl/lvgl ==9.5.0`，依赖声明在显示组件，锁文件由组件管理器生成。按用户选择从 9.6.0~1 降级；显式配置内置内存池 64KiB、断言头 `assert.h`，隐藏控件改用 `LV_OBJ_FLAG_HIDDEN`。未修改供应商源码。
- LCD：ESP-IDF `esp_lcd` ST7789 驱动，SPI2 / 20MHz / mode 2 / RGB565；方向、反色及字节序仍待实物确认。
- 触摸：FT6336 地址 `0x38`，使用共享 I²C master bus 和新 I²C API；官方驱动所标型号为 FT6336U，未将型号相近当作兼容性证据，因此实现本地有界读取。
- 当前触摸映射：`x=raw_y, y=239-raw_x`，原始坐标范围按 240×320。用户反馈原映射相对 UI 旋转 180°，据此修正；修正后的四角实际坐标尚未测量。
- 当前两个内部 DMA draw buffer 合计 51,200 bytes；`szpi_ui` 静态栈 6,144 bytes。

资源归属与释放顺序：

| 资源 | 所有者 | 容量 / 数量 | 销毁者及顺序 |
| --- | --- | --- | --- |
| SPI2 总线 | `szpi_display` | 单总线，最大传输 25,600 bytes | `szpi_display_deinit`；等 DMA 完成后删除 panel/io、释放总线，再释放 PCA9557 LCD_CS |
| panel IO / ST7789 panel | `szpi_display` | 各 1 个 | 先删除 panel，再删除 panel IO，随后释放 SPI 总线 |
| LEDC timer1/channel1 | `szpi_display` | 各 1 个 | UI stop 先设亮度 0，deinit 停止 channel 并 deconfigure timer |
| LVGL display / 两块 DMA buffer | `szpi_display` | display 1 个；buffer 各 25,600 bytes | UI 删除 indev 后，display 组件删除 LVGL display 并释放两块 buffer |
| DMA completion semaphore | `szpi_display` | 静态计数 semaphore，容量 2 | 静态生命周期，不动态销毁；每次初始化先清空旧计数 |
| FT6336 I²C 设备句柄 | `szpi_input` | 1 个，board 总线借用 | UI stop 在显示资源释放前删除设备句柄；不删除共享总线 |
| UI stack / status lock | `szpi_app` runtime | 静态栈 6,144 bytes；静态 mutex 1 个 | runtime 拥有静态生命周期 |

## 构建与配置

- LVGL 9.5.0 适配后执行 `idf.py build` 成功；生成配置确认内置池为 64KiB、断言头为 `assert.h`，本地启动色条诊断开启。当前镜像 `0x11af00` bytes，OTA 剩余 `0x6d5100` bytes（约 86%）。以下 9.6.0~1 镜像数据保留为历史记录，降级后的显示效果仍待上板确认。

- 主构建命令：`idf.py build`（ESP-IDF 6.1 环境）。结果：成功。
- 主构建应用镜像：`0x120a30` bytes；最小 OTA 应用槽：`0x7f0000` bytes；剩余 `0x6cf5d0` bytes（约 86%）。分区生成表中 `ota_0` / `ota_1` 各为 8128KiB。
- 独立 defaults 完整构建：`idf.py -B /tmp/szpi-phase2-defaults -D SDKCONFIG=/tmp/szpi-phase2-defaults/sdkconfig build`；配置目标为 ESP32-S3，Flash 16MiB、Octal PSRAM 80MHz，`LV_BUILD_EXAMPLES` / `LV_BUILD_DEMOS` 均关闭，Wi-Fi SSID / password 为空。构建成功，镜像 `0x120a10` bytes，OTA 余量 `0x6cf5f0` bytes（约 86%）。
- 当前生成的项目 `sdkconfig` 可能含本机 Wi-Fi 凭据；未写入本文或版本控制。
- 构建输出有 ESP-IDF 配置提示：`ESP_INT_WDT_TIMEOUT_MS` 当前值为 300ms，与 IDF Kconfig 默认值 800ms 不同，构建仍成功。该设置来自此前项目配置，本阶段未改动。

## 已执行的逻辑检查

- `cc -std=c11 -Wall -Wextra -Werror tests/test_szpi_input_transform.c -o /tmp/test_szpi_input_transform && /tmp/test_szpi_input_transform`：通过。覆盖四角、中心、原始坐标越界、空输出指针，以及双触点时保持主 ID、主触点释放后切换和全部释放状态。
- 尚未覆盖触摸寄存器读取失败 / RELEASED、稳定主触点选择、刷新区域边界、提交失败、DMA 完成与超时缓冲所有权、初始化失败逆序清理、服务重复启停和触摸降级。

## 尚未进行的上板验收

- 用户首次上板反馈：背光亮但页面不可见；启动日志显示 ST7789 驱动初始化成功、UI 任务存活，无刷新超时。GPIO42 的 LEDC 警告来自 board 预先配置该输出，不能据此判定背光故障。
- 排查修正：在 SPI panel IO 创建并配置 mode 2 后，才通过 PCA9557 拉低 LCD CS，避免选中期间配置 SPI 时钟。新增首帧完成及背光亮度日志；该修正是否解决空白屏仍待上板验证。
- 用户复测：首帧完成日志出现、背光亮，但仍无 UI；上述 CS 顺序调整未解决问题。增加可选 `CONFIG_SZPI_DISPLAY_STARTUP_COLOR_TEST`（menuconfig → SZPI display），默认关闭，当前本地 sdkconfig 开启以排查。启动直接发送红 / 绿 / 蓝 / 白横条，显示 3 秒后进入 LVGL；使用已有内部 DMA 缓冲并逐次等待完成，无新增任务。色条是否可见仍待用户反馈。

- 未烧录本阶段固件；没有颜色、旋转、反色、最后行列可见性或触摸坐标的实测结果。
- 未测背光 10% / 50% / 100%、启动及停止背光电平。
- 未测 Wi-Fi 状态展示、100 次点按、拖动、10 次启停、堆变化或连续运行 30 分钟。
- 坐标、颜色顺序、SPI 字节序及方向初值仍需按 `docs/hardware_io.md` 的说明逐项校准。

## 空白屏进一步排查

- 用户反馈降级至 9.5.0 后仍仅背光、未见彩条，不能记为显示通过。
- 增加 LCD CS 写入后的 PCA9557 输入、输出及方向寄存器日志；`requested=0 pin=0 output=0x04 config=0xf8` 为选中时预期值。读回仅验证扩展器端口，不能证明 J4 排线端实际信号。诊断读取失败单独报告，不改变输出影子写成功才更新的规则。
- 增加 SPI 频率覆盖和 mode 配置，默认仍为 board 20MHz / mode 2；当前本地 sdkconfig 使用 1MHz / mode 2，保留启动色条。menuconfig → SZPI display 可调整；频率设为 0 恢复 board 默认。尚无此配置的上板结果。

## 对照立创 LCD 官方例程的顺序修正

- 重新阅读 [官方第 9 章](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/lcd-display.html)：文档明确要求 reset 调用之后、panel init 之前拉低 CS。此前代码始终在 reset 前选中，前次只移到 SPI IO 创建后仍未满足此要求。
- 修正为 CS 释放 → 创建 panel → reset 调用 → CS 拉低 → panel init → invert → swap → mirror。无独立 reset GPIO，未驱动共用 RESET；CS 高期间的软件复位不会被屏幕接收，API 返回成功仅表示主控完成事务。该事务在选中屏幕前建立 SPI 时钟模式，与官方顺序一致；是否为此次空白屏根因仍待上板验证。
- 撤回无效的 1MHz 本地诊断覆盖，恢复 board 默认 SPI2 / 20MHz / mode 2；官方例程为 SPI3 / 80MHz，项目仍遵守既定 SPI2 资源安排。保留 DMA 完成等待和启动彩条，不复制例程中传输后立即释放缓冲的写法。

## 用户确认显示与触摸旋转修正

- 用户确认按官方 reset → CS 拉低 → panel init 顺序修正后已有显示；不再将显示基本可见性记为未验证。完整颜色、边缘、刷新与长期运行验收仍未完成。
- 用户反馈触摸旋转 180°，映射改为 `x=raw_y, y=239-raw_x`，只在 input 转换一次，未改变 LCD 方向。同步四角与中心测试；点按、拖动和边缘精度仍待用户复测。

## 当前上板反馈与诊断清理

- 用户确认显示和触摸效果正常，180° 触摸映射修正有效。该反馈不替代 100 次点按、30 分钟运行及启停等量化验收。
- 按用户要求移除启动彩条函数、三秒等待及 Kconfig 项，并清除本地生成配置中的旧选项。启动直接创建并刷新验证 UI，首帧完成后开启背光。上文彩条和空白屏状态为历史排查记录。

## 80MHz 与约 60Hz 刷新目标

- 按用户要求设置 SPI2 80MHz / mode 2（显式 CONFIG 覆盖，board 20MHz 回退值保留），LVGL 刷新周期 16ms。配置同时写入 sdkconfig.defaults 并同步本地 sdkconfig。
- FreeRTOS tick 从 100Hz 改为 1000Hz，UI 使用 4ms 的 xTaskDelayUntil 调度，通知仅非阻塞读取，避免额外等待；保留 runtime 唯一 UI 任务，触摸轮询仍为 20ms。
- 两个内部 DMA buffer 各从 20 行增至 40 行，总共 51,200 字节；保持单个在途刷新及 DMA 完成后缓冲归还规则，全屏分块从 12 减至 6。
- 16ms 是约 60Hz 的刷新调度目标，不是实测 FPS。80MHz 全屏像素传输理论耗时 15.36ms，实际还包含命令、绘制和调度；全屏动画帧率、80MHz 稳定性与长期运行仍待上板测量。
- 本配置执行 `idf.py build` 通过；生成头文件核对 SPI 80,000,000Hz、LVGL 16ms、FreeRTOS 1000Hz。应用镜像 `0x11af20` bytes，OTA 余量 `0x6d50e0` bytes（约 86%）。未烧录或测量实际 FPS。
