# 项目开发规则

## 依据与范围

- ESP-IDF 6.1，目标 `esp32s3`，嘉立创实战派 ESP32-S3 / ESP32-S3-WROOM-1-N16R8；16MiB Quad Flash、8MiB Octal PSRAM，均为 80MHz。
- 接线与默认参数以 `docs/hardware_io.md` 为依据；原理图 `docs/hardware_schematic.pdf` V1.0.1，型号参数来自 `docs/hareware_description.png`，摄像头 GC2145 由用户确认。
- 结构和长期规范见 `docs/code_architecture.md`。用户已授权查询资料并采用通用初始配置，常规方向 / 频率 / 槽映射在上板时验证，不作为开工前确认阻塞项。
- 第一阶段范围与验收见根目录 `task_phase_1.md`：基础资源、安全启动、FreeRTOS runtime、Wi-Fi STA / DHCP 及有限重连；实现时遵循其中任务顺序和停止 / 失败策略。
- 第二阶段范围与验收见 `docs/develop/task_phase_2.md`：ST7789、FT6336、LVGL 与单屏验证 UI。LVGL 绑定在 szpi_display，页面在 szpi_app；唯一 UI 任务由 runtime 创建，遵守 DMA 完成与缓冲归还规则。实现和构建已完成，逻辑测试与上板校准仍待完成，见 `docs/develop/phase_2_validation.md`。
- 第一阶段代码已落在启动装配、`szpi_board`、`szpi_app` 与 `szpi_wifi`；ESP-IDF 6.1 构建通过。逻辑测试与板上验收状态见 `docs/phase_1_validation.md`。规划中的后续组件仍按需求逐步创建，不把计划当成已实现，不添加空组件、占位任务或虚假成功接口。

- 第三阶段实现见 `docs/develop/task_phase_3.md` 与 `docs/develop/phase_3_validation.md`：GC2145 camera、QVGA RGB565 与 LVGL 预览已落代码，IDF 6.1 增量及干净 defaults 构建通过；逻辑测试和上板验证待完成。保持共享新 I²C、单帧 PSRAM、runtime 预览任务与明确的帧 / staging 所有权。

- 第二阶段附加任务见 `docs/develop/task_phase_2_extra.md` 与 `docs/develop/phase_2_extra_validation.md`：QMI8658A 与 BOOT 按键已实现并通过 IDF 6.1 构建和主机逻辑测试；仍待板上验证。适配归属 szpi_input，复用 board 新 I²C，由唯一 UI 任务轮询，独立故障降级。

## 结构与边界

- 依赖方向：`main -> szpi_app -> 外设适配 -> szpi_board`；各层可使用所需 ESP-IDF / 供应商库，禁止循环依赖。
- main 只负责启动装配；业务流程和持续任务属于 szpi_app。跨 camera、display、audio、storage 的协作放服务层，不让外设组件互相编排。
- szpi_board 管理固定接线、共享 I²C、PCA9557、安全输出状态和资源绑定，不依赖上层外设组件。
- 外设组件按功能创建：szpi_display、szpi_input、szpi_camera、szpi_audio、szpi_storage。先按文件组织小驱动；实际复用或复杂生命周期出现后再拆组件。
- 组件公共契约放 include，私有实现与头文件放 src。供应商驱动优先复用，不手工修改 managed_components，不建立无实际需求的通用 HAL / 插件 / 事件框架。

## 硬件资源

- GPIO / 地址 / 扩展器 bit 的固定值只定义在 board，适配层读取 bindings，上层只调用语义接口。主控 GPIO 与 PCA9557 端口不可混用。
- GPIO1/2 的 I²C 只安装一次，由 board 拥有；GC2145 SCCB 复用兼容的已有总线。引入 camera 依赖时核对 IDF 6.1 兼容性，不能混装新旧 I²C 或重复安装同一总线。
- 扩展器输出影子值加锁，修改一位保留其他位；I²C 写成功后才更新影子值。初始 CS 释放、功放关闭、摄像头掉电，先写输出锁存值再设置方向。
- LCD 使用 SPI2；背光 LEDC timer1/channel1，camera XCLK timer0/channel0。音频统一管理 I2S_NUM_0 及共享 MCLK/BCLK/WS，ADC/DAC 不各自安装时钟源。
- GPIO35/36/37 保留给 PSRAM；GPIO0/3/45/46 为启动配置脚；GPIO10/11 仅按实际外部功能需求占用。
- 触摸 / IMU 采用轮询，SDMMC 只用 1-bit，没有插卡检测 GPIO；LCD / camera 没有独立 reset GPIO，不操作共用 RESET 来复位单个外设。

## FreeRTOS 统一管理

- 使用 ESP-IDF FreeRTOS SMP；项目任务由 szpi_app 的 runtime 唯一创建 / 启停入口管理。任务名、入口、栈、优先级、核亲和、启用条件集中在任务表，预算见 code_architecture.md。
- 任务入口由服务提供，不在业务 / 外设文件里散落 xTaskCreate*。供应商库内部任务记录为依赖任务例外，避免重复任务；默认无核绑定，调整需实测依据。
- queue / mutex / event group / timer / notification 与缓冲登记用途、容量、所有者和销毁者；在所属组件创建，runtime 协调生命周期。登记不是全局共享 handle 仓库。
- 默认长期任务及同步对象优先静态创建，任务栈放内部 RAM；IDF 栈大小参数以字节计，静态栈数组正确换算元素数。创建失败逆序清理。
- 共享状态用 FreeRTOS mutex，消息用有界 queue，单任务完成 / 唤醒用 notification，能力状态用 event group；周期任务使用 xTaskDelayUntil，不忙等。timer 回调仅通知，ISR 按 API 使用 FromISR。
- 服务协作停止：通知 STOP、停止生产、收尾在途操作、归还缓冲、确认停止后才能释放资源。禁止强删持锁或执行中的任务；队列中的指针内存需显式回收。
- supervisor 集中观察栈余量、堆、队列峰值及 overrun / timeout；重试与恢复有限且可观察。健康 watchdog 由任务有效进展维护，不能由 supervisor 替卡住任务喂狗。

## 接口、并发与错误

- 对外返回 esp_err_t；接口明确超时、线程安全、生命周期、所有权。init 失败逆序清理本次资源；重复 init 不重复安装；stop 等待在途操作完成后 deinit。
- 可恢复外设错误向上返回，驱动深处不调用 ESP_ERROR_CHECK 重启。核心故障明确报错，可选外设可降级，不能静默伪造成功。重试必须有上限。
- 相机帧必须 acquire/release 成对，归还后不得访问；LCD DMA 完成前不得覆写 / 释放缓冲。异步预览完成刷新后再归还帧。
- 任务与队列按持续处理需求创建；队列有固定容量及满队列策略。预览可丢旧请求，录音不能静默丢块。ISR / DMA 回调仅通知，不执行 I²C / 文件 I/O / UI 更新。
- UI 操作集中在所属任务。持总线 / 扩展器锁不等待其他服务或调用上层回调；不靠共享可变全局指针传数据。
- 帧 / 大缓冲优先显式 PSRAM，控制和实时小缓冲优先内部 RAM；DMA 内存按实际驱动能力 / 对齐申请，检查分配失败，减少稳定运行中的反复分配。

## 配置、风格与验证

- C、snake_case、组件前缀 szpi_*、类型后缀 _t、4 空格缩进；实现内符号尽量 static，使用模块 TAG 与 ESP_LOG，头文件自包含。
- CMake 明确列出源文件与依赖；公共头需要的依赖用 REQUIRES，否则 PRIV_REQUIRES。保留 MINIMAL_BUILD ON 和 esp_psram 构建依赖。
- 外部依赖在所属 idf_component.yml 声明已验证版本；保留生成的 dependencies.lock，升级依赖单独验证，不手改 lock。
- 固定接线在 board，可调参数按需用 CONFIG_SZPI_* / Kconfig，运行设置放 NVS。sdkconfig.defaults 为持久配置，sdkconfig 为生成文件且被忽略；改 defaults 后同步生效配置并验证。
- partitions.csv 为双 OTA、无 factory，两槽等大，各 8128KiB，占满应用可用 Flash；不擅自添加 Flash 文件系统。大文件放 SD，小配置放 NVS，禁止自动擦除配置作为通用恢复手段。
- OTA 使用官方 API 查找非运行分区，不硬编码偏移；开启回滚必须同时实现启动健康确认。
- 代码 / 配置 / 依赖改动运行 idf.py build；分区改动核对生成表容量与对齐。纯文档变更检查内容与链接即可，不运行无关构建。
- 测试优先覆盖状态、失败清理、缓冲所有权、影子寄存器及队列边界；避免为机械常量写镜像测试。编译通过不等于上板通过。
- 修改接线、资源、默认参数、结构或公共契约时同步相关文档与 README。烧录、擦除和设备操作遵循用户当前授权范围。
