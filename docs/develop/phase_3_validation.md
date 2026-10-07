# 第三阶段验证记录

日期：2026-10-07。状态：**实现与 IDF 6.1 增量 / 干净 defaults 构建已通过；逻辑 / 上板验收未完成**。

## 实现范围

- `szpi_board` 增加 GC2145 DVP、XCLK、SCCB 地址和 PID bindings；固定引脚只由 board 定义。
- `szpi_camera` 固定依赖 `espressif/esp32-camera == 2.1.8`，初始化借用 board 的 I2C_NUM_0，使用新 SCCB 后端、GPIO5 / 20MHz XCLK、QVGA RGB565、单 PSRAM framebuffer 和 `CAMERA_GRAB_WHEN_EMPTY`。适配拒绝非 GC2145 PID、非 QVGA / RGB565 / 153600 字节帧及非 PSRAM 缓冲；跟踪单个 outstanding frame、token 和获取任务，禁止错误 / 重复归还及持帧 deinit。
- `szpi_app` 增加 runtime 创建的 `szpi_preview`（4096 字节内部静态栈、优先级 4、无核绑定）、容量 1 帧队列和容量 1 确认队列。UI 任务把帧复制到独立 PSRAM staging；采集任务收到 generation + token 匹配的确认后，才调用 camera release。
- UI 新增 Camera 页及 Start / Stop / Back。显示实际采集与刷新窗口 FPS、帧尺寸、错误数；记录最大采集、复制和刷新耗时。camera 输出按 RGB565 高字节优先处理（依据锁定版本 [esp32-camera RGB565 编码器说明](https://github.com/espressif/esp32-camera/blob/v2.1.8/conversions/include/img_converters.h)），复制到 LVGL staging 时交换每像素两个字节，以匹配 ESP32 小端 RGB565 内存格式，并逐行水平镜像前置预览；sensor 默认方向与 LCD 显示配置不变。返回或 UI 停止会先停止预览并排空帧，再解绑图像源、等待 LCD DMA 后释放 staging。
- 预览最多 3 次连续采集错误后进入可观察故障状态；明确 Start 可再次尝试。停止等待供应商采帧调用的 4 秒上限，预算为 5 秒；若 UI 确认超时则保留 camera 帧和引用资源，不强删任务或提前释放。

## 供应商源码核对

锁定的 v2.1.8 源码检查结果：

- [组件依赖清单](https://github.com/espressif/esp32-camera/blob/v2.1.8/idf_component.yml)要求 ESP-IDF >=5.1，并公开依赖 `esp_jpeg ^1.3.1`。
- [CMake](https://github.com/espressif/esp32-camera/blob/v2.1.8/CMakeLists.txt)在 IDF >=6.0 使用 `esp_driver_gpio`、`esp_driver_spi`、`esp_driver_i2c`，并选择 `driver/sccb-ng.c`（条件为 IDF >=5.4 且未选 legacy）。源码包含 GC2145 和 ESP32-S3 低层驱动。
- [新 SCCB 实现](https://github.com/espressif/esp32-camera/blob/v2.1.8/driver/sccb-ng.c)通过 `i2c_master_get_bus_handle(port)` 取得已安装总线，按探测地址添加自身 device。设置为已有端口后 `sccb_owns_i2c_port=false`；deinit 删除 SCCB device 后直接返回，不删除共享 bus。驱动 init 失败会进入 `esp_camera_deinit()`，同样清理 SCCB 设备而保留 board bus。
- [camera driver](https://github.com/espressif/esp32-camera/blob/v2.1.8/driver/esp_camera.c)将 `esp_camera_fb_get()` 的固定等待设为 4000ms。`cam_hal.c` 创建供应商任务 `cam_task`，默认栈 4096 字节、优先级 `configMAX_PRIORITIES - 2`；项目 defaults 选择无核绑定。默认关闭 PSRAM DMA，帧缓冲仍显式放 PSRAM。GC2145 sensor 配置支持 RGB565，PID 从 `sensor->id.PID` 读取。

源码检查只证明接口和清理路径的设计依据。实际组件构建仍是确认 IDF 6.1 兼容的门槛。

## 构建与依赖

- 依赖声明：`components/szpi_camera/idf_component.yml` 固定官方 Git tag `v2.1.8`；registry 网页有该版本，但本机 Component Manager 镜像 `components-file.espressif.com` 对组件 JSON 返回空版本清单。用 Git tag 固定源码来源；未手改 `dependencies.lock` 或 `managed_components`。
- 配置 defaults 增加新 SCCB 后端、I2C0、GC2145、4096 字节供应商任务、无核绑定及 PSRAM DMA 关闭。
- 本机 registry 解算失败：组件文件镜像返回空版本列表；依赖因此固定官方 `v2.1.8` Git tag，由组件管理器生成 lock。
- 在 IDF 6.1 环境执行 `idf.py build` 增量构建通过；二次增量构建在 runtime 状态返回修正后也通过。再以独立目录 `/tmp/szpi-phase3-clean` 和空白 sdkconfig 执行 `idf.py -B /tmp/szpi-phase3-clean -D SDKCONFIG=/tmp/szpi-phase3-clean/sdkconfig build`，全量干净构建通过。
- 干净 sdkconfig 中 GC2145、新 SCCB I²C driver、I2C0、camera task 4096 bytes、无核绑定均生效；PSRAM DMA 保持关闭。RGB565 帧 staging 字节序转换及前置预览水平镜像已加入。当前固件 `0x131890` bytes，最小 OTA app 槽 `0x7f0000` bytes，剩余 `0x6be770` bytes（约 85%）。分区表仍为两个各 8128 KiB 的 OTA 槽。
- 首次编译发现并修复 FreeRTOS include 顺序；复构建无项目编译错误。干净构建中 ESP-IDF 自身仍打印若干已有跨组件私有 include 检查警告。
- 当前未运行逻辑测试；项目规则要求的 init 失败清理、共享 bus、PWDN 影子位、坏帧 / 重复 release、队列与迟到 generation、staging / DMA 所有权及故障降级用例仍待覆盖。

## 尚未上板

用户反馈显示本身正常、camera 预览颜色异常；已在 staging 拷贝中增加 RGB565 每像素字节交换，重新构建通过。没有烧录或访问设备，修正效果尚未实物确认；若颜色仍不对，下一步检查 RGB/BGR 元素顺序及 DVP 数据线位序。以下结果均未测：探测 PID=0x2145、100 帧有效采集、画面方向、Start / Stop 和页面切换 10 次、Wi-Fi 连接变化时的预览、FPS 与最大延迟、堆和 DMA 连续块、任务栈余量、30 分钟运行及停止后掉电 / 重启。
