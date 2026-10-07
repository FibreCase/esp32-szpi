# 第一阶段验证记录

日期：2026-10-07

## 当前结果

- **代码实现：** 启动诊断、board/I²C/PCA9557、FreeRTOS runtime、Wi-Fi STA 生命周期与有限重试已实现。
- **ESP-IDF 构建：** 通过 ESP-IDF 6.1 完整构建。应用镜像为 `0xC6CD0` 字节；最小 OTA 应用槽 `0x7F0000`，剩余 `0x729330` 字节（90%）。项目源文件未产生编译警告。
- **分区：** 官方 `gen_esp32part.py --flash-size 16MB` 校验通过。`ota_0` 为 `0x20000 / 0x7F0000`，`ota_1` 为 `0x810000 / 0x7F0000`，末尾 `0x1000000`，无 factory 分区。
- **干净 defaults 配置：** 独立目录 `/tmp/szpi-phase1-defaults` 的 `sdkconfig` 生成成功，确认 `esp32s3`、16MB QIO Flash、80MHz、Octal PSRAM 80MHz、custom 双 OTA 布局；`CONFIG_SZPI_WIFI_ENABLED=y`，SSID / 密码均为空。
- **逻辑状态测试：** 尚未运行自动化状态机、队列边界或 PCA9557 失败清理测试。构建通过不代表这些运行时路径已验证。
- **单次上板正常路径：** 2026-10-07 启动日志确认从 `ota_0` 启动，识别 16MiB Flash / 8MiB PSRAM，PSRAM memory test 通过；PCA9557 输出 / 极性 / 方向读回为 `0x05 / 0x00 / 0xF8`；STA 连接 WPA2 AP 并获取 DHCP IPv4 `192.168.31.180`。日志至少持续到启动后约 20 秒并输出 supervisor 堆 / 栈统计。未记录 SSID。
- **尚未覆盖的硬件场景：** 实际安全引脚电平 / 外设关闭效果、无效密码、AP 不可达、DHCP 不响应、断线重连、窗口耗尽后 RETRY、主动 STOP、重复启停和 30 分钟运行仍未验证。

核心存储、PSRAM、NVS 或 board 初始化失败时，代码会尝试只启动 supervisor 并发布 `SYSTEM_FAULT`；若 runtime 本身无法创建，同样会记录明确启动错误并停止后续启动。

## 构建环境提示

本机 IDF 环境需要设置 `IDF_PYTHON_ENV_PATH` 并使用 `IDF_PYTHON_CHECK_CONSTRAINTS=0` 才能激活；CMake 组件管理器还需要读取 macOS 进程列表。完成上述环境设置及访问后，`idf.py build` 成功。

构建输出包含 ESP-IDF 自身的 `esp_wifi` / `wpa_supplicant` 组件私有头文件校验警告，以及已有 `sdkconfig` 中 `CONFIG_ESP_INT_WDT_TIMEOUT_MS=300` 与 IDF Kconfig 默认 800 不一致的提示；这些不是本阶段源文件编译警告。本阶段没有改变 watchdog 默认值。

## 上板验收状态

正常联网冷启动结果已记入 `task_phase_1.md`。后续按该文件上板表执行失败路径、停止 / 重试、重复启停及连续运行用例后，在此补录结果。凭据不要写入此记录。
