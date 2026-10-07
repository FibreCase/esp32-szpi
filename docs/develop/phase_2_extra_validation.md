# 第二阶段附加任务验证记录：QMI8658A 与 BOOT

日期：2026-10-07

## 软件验证

- 目标：ESP32-S3，ESP-IDF 6.1；项目依赖版本见 `dependencies.lock`。
- 输入驱动：`szpi_input` 内本地 QMI8658A 小型寄存器驱动和 GPIO0 消抖状态机；借用 board I²C0，不安装或删除共享总线。
- 生命周期：IMU、BOOT、FT6336 分别初始化和清理。可选输入初始化失败只记录故障，不阻止显示、触摸、camera 或 Wi-Fi。IMU 在唯一 UI 任务每 20ms 轮询，BOOT 每 4ms 采样；验证页每 100ms 更新。
- 样本同步：使用 QMI8658A Rev A STATUSINT 数据锁定流程，一次从 AX_L 连续读取 12 字节；小端显式解码。配置加速度 ±4g / 250Hz，陀螺仪 ±512dps，六轴同步 ODR 为 224.2Hz；倾角只有静态 roll / pitch。
- 主机测试：`cc -std=c11 -Wall -Wextra -Werror tests/test_szpi_input_extra.c -lm -o /tmp/test_szpi_input_extra && /tmp/test_szpi_input_extra` 通过，覆盖按键抖动、短按 / 长按、启动时按住、计时回绕，以及 IMU 字节解码、灵敏度和倾角有效性。
- 固件构建：`source /home/fibre/.espressif/v6.1/esp-idf/export.sh && idf.py build` 通过。一次沙箱内构建因组件管理器缓存位于工作区外而无法写入，授权访问缓存后同一构建成功。
- 干净 defaults：使用独立目录 `/tmp/esp32-szpi-phase2-extra-defaults` 和独立 SDKCONFIG 构建通过；确认从根目录 `sdkconfig.defaults` 生成配置并完成全量构建。

## 上板验证

以下项目尚未执行，不能由构建或主机测试推断硬件已通过：

- [ ] WHO_AM_I=0x05、静置加速度约 1g、陀螺仪偏置与三轴符号。
- [ ] BOOT 短按 / 长按计数、GPIO0 启动与下载模式。
- [ ] 输入页与触摸、camera 预览、Wi-Fi 并行稳定性。
- [ ] 启停循环、故障降级、I²C 最大耗时、UI overrun、堆和任务栈余量。
