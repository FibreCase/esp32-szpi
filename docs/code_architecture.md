# 代码结构与开发规则

## 适用范围与当前状态

本方案针对嘉立创实战派 ESP32-S3、ESP-IDF 6.1，硬件接线及初始参数以 [hardware_io.md](hardware_io.md) 为准。产品功能尚未具体定义，因此先建立显示、输入、摄像头、音频和存储能力的边界，业务功能按实际需求添加。

**第一阶段已实现正式启动、board、runtime 和 Wi-Fi，并记录单次正常联网启动；完整验收尚未完成。第二阶段已实现 ST7789 / FT6336 适配、LVGL 单屏 UI 与 runtime 启停，并通过构建；逻辑测试及板上方向、颜色和触摸校准仍待完成。** 任务与验证记录分别见 [第二阶段任务](develop/task_phase_2.md) 和 [第二阶段验证](phase_2_validation.md)。下文列出的后续功能继续按需求逐步落地，不预先添加空组件、虚假成功的 API 或占位任务。

## 分层与依赖

| 层次 | 位置 | 职责 | 禁止承担的职责 |
| --- | --- | --- | --- |
| 启动装配 | main | 选择功能、安排启动顺序、处理启动结果 | 芯片寄存器操作、硬编码引脚、持续采集循环 |
| 应用服务 | components/szpi_app | 预览、录音、设置、OTA 等具体流程；任务及业务状态 | 直接操作 GPIO、I²C、PCA9557 或管理底层总线 |
| 外设适配 | components/szpi_display 等 | 把供应商驱动适配为本板可用的能力接口 | 页面逻辑、文件命名策略、网络协议、跨外设业务编排 |
| 板级资源 | components/szpi_board | 引脚绑定、I²C 生命周期、扩展器控制、安全启动状态、硬件资源分配 | 创建 UI、摄像头采集任务、音频业务任务或依赖上层外设组件 |
| 基础驱动 | ESP-IDF / managed_components / 必要的私有驱动 | SPI、I²S、SDMMC、ST7789、GC2145、音频芯片等实现 | 项目业务策略 |

依赖只能向下：`main -> szpi_app -> 外设适配 -> szpi_board`，各层还可依赖所需 ESP-IDF / 供应商库。跨外设协作放在 `szpi_app`，例如摄像头预览由服务同时调用 camera 与 display，而不是 camera 组件依赖 display。板级组件不调用各外设 init，以免形成循环依赖。

外设适配组件可以被 main 的单项硬件验证流程直接调用；这不改变依赖方向。暂不建立通用 HAL、插件注册系统或跨层事件总线。第一阶段已有明确 Wi-Fi 需求，增加 szpi_wifi 适配组件与 szpi_app 内的 wifi_service；具体范围见 [第一阶段任务](../task_phase_1.md)。

## 目标目录

下面列出的目录与文件为计划名称，按实现阶段逐步落地。

```text
esp32-szpi/
├── AGENTS.md                         # 自动化编码规则的入口
├── CMakeLists.txt                    # 项目与最小组件构建
├── sdkconfig.defaults                # 可复现的项目配置
├── partitions.csv                   # 双 OTA，无 factory，占满 16MiB
├── dependencies.lock                # 组件管理器生成，添加依赖后保留
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild             # 需要时添加项目级功能开关
│   ├── app_main.c                    # 调用启动 / 应用入口
│   └── app_boot.c                    # 功能装配、启动结果与降级
├── components/
│   ├── szpi_board/
│   │   ├── CMakeLists.txt
│   │   ├── include/
│   │   │   ├── szpi_board.h          # 基础初始化、状态查询
│   │   │   ├── szpi_board_bindings.h # 仅供外设适配使用的接线 / 资源接口
│   │   │   └── szpi_board_control.h  # LCD CS、功放、摄像头掉电语义接口
│   │   └── src/
│   │       ├── szpi_board.c
│   │       ├── szpi_board_pins.h     # 唯一固定 GPIO / 地址 / 扩展器位定义
│   │       ├── szpi_board_i2c.c      # 共享总线与设备接入
│   │       ├── szpi_board_control.c  # 输出影子值、同步与极性转换
│   │       └── pca9557.c             # 仅在没有合适现成驱动时实现
│   ├── szpi_display/                 # ST7789、SPI、背光、异步刷新
│   ├── szpi_input/                   # FT6336、BOOT 键、QMI8658A
│   ├── szpi_camera/                  # GC2145、帧借用 / 归还、PWDN
│   ├── szpi_audio/                   # ES7210、ES8311、共享 I²S / 时钟、功放
│   ├── szpi_storage/                 # SD 挂载、NVS 设置与文件访问约束
│   ├── szpi_wifi/                    # 第一阶段 STA / netif / 事件适配
│   └── szpi_app/
│       ├── include/szpi_app.h
│       └── src/
│           ├── szpi_app.c            # 应用状态与服务启动
│           ├── szpi_runtime.c        # FreeRTOS 任务统一启停与监督
│           ├── szpi_task_table.c     # 任务资源与调度参数集中定义
│           ├── szpi_resource_table.c # RTOS 对象及缓冲预算登记
│           ├── preview_service.c    # 出现预览需求时实现
│           ├── recording_service.c  # 出现录音需求时实现
│           ├── settings_service.c   # 出现设置需求时实现
│           └── ota_service.c        # 出现 OTA 需求时实现
├── managed_components/              # 自动下载，不手工修改
├── docs/
│   ├── hardware_schematic.pdf
│   ├── hareware_description.png
│   ├── hardware_io.md
│   └── code_architecture.md
└── tools/                            # 出现实际需要时添加验证 / 资源转换工具
```

每个实际创建的组件至少包含 `CMakeLists.txt`、`include/<组件名>.h`、`src/`。私有头文件放 `src/`，只有对外契约才放 `include/`。FT6336、按键、IMU 先在 `szpi_input` 内按文件组织；只有出现独立复用或复杂生命周期时再拆组件。第二阶段明确引入 LVGL，绑定放在 `szpi_display` 内，页面及交互流程属于 `szpi_app`。

优先使用官方驱动：ST7789 用 ESP-IDF esp_lcd，GC2145 用 espressif/esp32-camera，音频优先评估 espressif/esp_codec_dev。FT6336、QMI8658A、PCA9557 评估现成组件的具体器件与 IDF 兼容性，缺失时实现小范围驱动，不复制其他开发板的引脚表。

## 资源归属

| 资源 | 唯一管理者 | 规则 |
| --- | --- | --- |
| GPIO / I²C 地址 / 扩展器 bit | szpi_board | 固定值只定义在板级层，外设适配读取 bindings，上层使用语义 API |
| GPIO1/2、I2C_NUM_0 | szpi_board | 一次安装；所有 I²C / SCCB 器件接入同一总线；不混装新旧 I²C 驱动 |
| PCA9557 | szpi_board | 锁保护输出影子值；单 bit 更新保留其他输出；写失败不更新影子值 |
| SPI2_HOST、LCD DC/MOSI/SCLK | szpi_display | 初始化、DMA 传输及完成回调由该组件管理 |
| LCD 背光 LEDC timer1/channel1 | szpi_display | 资源编号由 board 提供，不与 XCLK 共享 |
| GC2145 XCLK timer0/channel0 | szpi_camera | 由 camera 驱动管理，不被背光或蜂鸣用途重新配置 |
| I2S_NUM_0、MCLK/BCLK/WS | szpi_audio | 一套统一时钟 / 格式配置，ADC/DAC 不各自安装控制器 |
| SDMMC 单线接口 | szpi_storage | 独占挂载生命周期；卡访问错误从接口返回 |
| 8MB PSRAM | 各缓冲所属组件 | 每块内存有明确生命周期；不因全局 malloc 可用就假定 DMA 兼容 |
| 双 OTA 与 otadata | OTA 服务 + ESP-IDF OTA API | 根据分区子类型查找，禁止硬编码目标 slot 偏移 |
| GPIO10/11 扩展接口 | szpi_board 记录分配 | 只有实际需求才占用，不能与另一功能同时驱动 |

`szpi_board_control` 对外使用语义，如 `szpi_board_camera_power_down(bool)`、`szpi_board_amplifier_enable(bool)`、`szpi_board_lcd_select(bool)`；调用者不传扩展器 bit，也不负责低有效转换。上述函数名是拟定接口，不代表已经实现。I²C handle 是借用资源，外设适配可创建设备句柄，但不能删除板级总线；上层服务不获取原始总线句柄。

GC2145 复用 I²C 的具体接口必须在引入依赖时核对。若所选版本只能使用与本项目不兼容的 I²C 后端，应更换兼容版本或在 camera 适配内实现受控桥接；不以安装第二套 GPIO1/2 总线绕过冲突。

## 生命周期、接口与错误规则

- 状态至少区分未初始化、可用、运行中、故障；不用一串不相关全局 bool 表达互斥状态。外设重复 init 不得重复申请总线 / 通道，明确返回已有实例或 `ESP_ERR_INVALID_STATE`。
- 返回 `esp_err_t`，数据用输出参数或明确的借用对象；所有可阻塞接口带超时或记录明确的最大阻塞时间。公开头文件说明线程安全、调用上下文、资源所有权和返回约束。
- `init` 失败只释放本次成功创建的资源，按逆序清理；`stop` 先停止生产数据并等待在途操作完成，`deinit` 再释放资源。
- 摄像头帧从 acquire 到 release 期间归借用者使用，归还后不得再访问；多消费者必须拷贝或实现明确引用计数，不能共享一个无约束指针。
- 显示 DMA 使用的缓冲在完成回调前不能释放 / 覆写。相机帧直接用于异步 LCD 刷新时，也必须等刷新结束再归还相机。
- 普通可恢复外设错误返回上层，不能在驱动深处调用 `ESP_ERROR_CHECK` 重启整机；它仅用于启动装配中明确认定必须终止的系统不变量。
- 重试有次数与退避上限，不能无限等待设备响应。错误日志说明设备、操作和 err；不重复打印每帧 / 每样本日志。
- 核心存储配置或必需基础资源失败进入故障状态；可选外设失败可以降级，但必须记录能力不可用，不能返回成功或伪造数据。

## FreeRTOS 统一任务与资源管理

### 管理方式

**使用 ESP-IDF 自带的 FreeRTOS SMP，所有项目任务由 `szpi_app` 内的 runtime 统一管理。** FreeRTOS 提供调度与同步；项目仍负责硬件句柄、缓冲和文件的所有权、生命周期与预算。不要假定删除任务会自动释放这些资源。

使用 ESP-IDF 已启动的调度器；不引入第二套 FreeRTOS，不自行调用 vTaskStartScheduler。runtime 先保持为几个明确文件，不额外创建通用调度框架：

```text
components/szpi_app/src/
├── szpi_runtime.c       # 启动 / 停止顺序、监督任务、故障与能力状态
├── szpi_task_table.c    # 任务名、入口、栈、优先级、核亲和、启用条件
├── szpi_resource_table.c # RTOS 对象 / 缓冲归属、容量与诊断登记
└── szpi_runtime_internal.h
```

- 项目 task 创建集中在 runtime，服务提供 task 入口与上下文；服务和外设适配不能各自散落 `xTaskCreate*`。ESP-IDF / camera / LVGL 等库内部任务属于已登记的依赖任务例外，记录用途与配置，避免重复创建相同功能任务。
- FreeRTOS mutex、queue、event group、通知、timer 等对象由其资源所有者创建，runtime 统一登记名称、用途、容量、创建 / 销毁者和失败清理规则。登记表不是全局可随意访问的 handle 仓库。
- board 仍是总线和扩展器唯一所有者，audio 仍是 I²S 唯一所有者。runtime 按依赖顺序调用它们的生命周期接口，通过 FreeRTOS 同步原语协调访问，不接管每次硬件传输。
- 启动顺序：基础配置 / board -> RTOS 通信对象与缓冲 -> 启用的外设 -> 服务任务 -> 能力状态发布。任何一步失败都能逆序释放本次创建的对象，未初始化完成的服务不运行。
- 应用任务需要的资源在启动阶段分配并核对预算；队列 / 事件组可按需采用静态创建，长期固定任务优先静态栈 / TCB，栈放内部 RAM。动态创建仅用于实际按需生命周期，创建失败向启动流程返回。

### 初始任务表

任务只在相应功能启用时创建。以下为首次实现的预算起点，属于项目选择；栈使用量、延迟和负载测量后调整。优先级数值越大越高，必须小于项目 `configMAX_PRIORITIES`。

| 任务 | 启用条件 | 初始优先级 | 初始栈预算（字节） | 等待方式 / 职责 |
| --- | --- | --- | --- | --- |
| szpi_supervisor | 正式运行流程 | 3 | 4096 | 阻塞等控制 / 故障队列，周期采集资源状态 |
| szpi_wifi | Wi-Fi 启用且有配置 | 4 | 4096 | 阻塞消息等待，执行连接 / 超时 / 有限重试 |
| szpi_ui | 显示 / 输入启用 | 5 | 6144 | 输入 / UI 周期等待，统一 UI 调用 |
| szpi_preview | 摄像头预览启用 | 4 | 4096 | 阻塞采集与显示完成通知，管理帧归还 |
| szpi_audio_rx | 录音启用 | 8 | 6144 | 阻塞 I²S 读取，把有效音频块交给消费者 |
| szpi_audio_tx | 播放启用 | 8 | 6144 | 阻塞队列 / I²S 写入，维持播放数据流 |
| szpi_storage | 持续写卡启用 | 6 | 4096 | 阻塞文件请求队列，负责录制文件写入 |
| szpi_ota | OTA 功能启用 | 2 | 8192 | 阻塞工作请求，下载 / 校验 / 写入升级 |

默认 `tskNO_AFFINITY`，由 SMP 调度；如供应商 API 有亲和要求或测量发现实时抖动，再在任务表集中设置绑定核。不得在业务文件里随意更改优先级 / 核绑定。实时任务每轮必须阻塞或等待，禁止高优先级忙循环压住 idle / 系统任务。

ESP-IDF 的任务栈参数以字节计，与原生 FreeRTOS 常见的 word 单位不同；静态创建时栈数组按 `StackType_t` 元素正确换算。任务入口不能直接 return；任务的创建 / 退出 / 回收遵循 [ESP-IDF FreeRTOS 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html)。

### 同步原语选型

| 用途 | FreeRTOS 原语 | 使用规则 |
| --- | --- | --- |
| 扩展器影子值、跨寄存器原子操作、共享状态 | Mutex | 使用带优先级继承的 mutex；规定锁持有范围与超时，不以 binary semaphore 代替互斥 |
| 控制命令、按键 / 输入事件、文件请求 | 有界 Queue | 消息注明类型 / 所有权；固定深度、发送超时和满队列策略 |
| DMA 完成、单任务唤醒 / 停止请求 | Task Notification | 通知 index / bit 集中定义；计数型通知与 bit 型协议不能混用 |
| 系统就绪、功能可用、服务停止状态 | Event Group | 只表示状态，不传数据，不用于要求逐条处理的命令 |
| 音频单生产者 / 单消费者字节流 | Stream Buffer 或固定块队列 | 多生产者 / 多消费者场景不用无保护 Stream Buffer；块队列显式交接缓冲 |
| 周期轮询 / 低频监督 | 周期 delay 或 Software Timer | 周期任务使用 `xTaskDelayUntil` 与 `pdMS_TO_TICKS`；timer 回调只通知任务 |

不在软件 timer 回调中执行触摸 I²C、写 SD 或耗时采集。ISR / DMA 回调使用允许的 FromISR API，按要求触发调度；阻塞任务定期等待超时用于处理停止请求，不能永久卡在底层调用导致无法停机。mutex 不在 ISR 使用。

### 生命周期与资源安全

- runtime 控制服务状态：`STOPPED -> STARTING -> RUNNING -> STOPPING -> STOPPED`，错误进入 `FAULT`；服务状态与 FreeRTOS 的 Ready / Blocked 等调度状态分开记录。
- 停止采用协作协议：发送 STOP -> 唤醒任务 -> 停止生产 -> 排空或取消在途数据 -> 归还缓冲 / 文件收尾 -> 发布停止确认。确认之前不释放依赖资源。
- 默认长期任务停止服务后进入阻塞等待，可再次启用；任务退出回收仅在已清理资源、没有持锁 / 在途 DMA 的明确状态执行。不得从其他核强行 `vTaskDelete` 一个运行中的任务来“恢复”。
- 队列销毁前先停生产者 / 消费者，并释放队列中尚未消费的缓冲；FreeRTOS 不会自动释放指针消息指向的内存。销毁之后清空登记 handle，禁止悬空引用。
- 串行化设备控制由所属组件完成；runtime 控制资源可用状态，不把全部操作塞进一个全局大锁，也不把全部 I²C、SPI、I²S 操作转发给 supervisor。
- 应用队列携带数据描述符而非大块图像复制；借用帧只有一个归还责任人。UI 刷新与相机预览共享面板时，通过 display 的串行提交 / 完成接口协调，不能同时调用底层 panel API。

### 预算与健康监控

任务 / 资源表同时记录内部 RAM 栈预算、PSRAM 帧缓冲预算、队列深度、音频块大小及最大数量。启动时检查分配结果并输出一次摘要；库内部任务 / 缓冲计入总预算，动态内存也保留在原组件归属下。

supervisor 低频观察栈余量、内部 / PSRAM 最小剩余堆、队列峰值、音频 underrun / overrun、丢弃预览帧与 I²C 超时计数。按预算阈值报告异常，只在明确实现的恢复流程中重启对应服务；不能无限自动重启掩盖故障。FreeRTOS 调度状态不等于业务健康，长期阻塞等待输入是正常行为。

Task Watchdog 由任务在约定的有效进展点自行维护，或用适当的 watchdog user 监控操作进展；supervisor 不能替失去进展的任务统一喂狗。设置超时应覆盖该任务正常最大阻塞时间，同时保证 idle 有运行机会。参见 [ESP-IDF Watchdog 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/wdts.html)。

验收新增：反复启动 / 停止服务后没有持续堆泄漏；队列满、I²C 超时、SD 写入变慢和相机帧异常时能够按策略退出 / 降级；实时任务不饿死其他任务；记录实际栈余量再调整初始预算。

## 并发与数据流

组件 init 默认同步执行；只有真实的持续处理需求才创建任务。任务入口放所属服务中，由 runtime 按任务表统一创建；底层库已有任务时不再无目的加一层任务。核绑定、栈和优先级集中管理，初始预算以上表为准，随后按实测调整。

| 场景 | 初期组织 | 流量与资源规则 |
| --- | --- | --- |
| UI / 输入 | 一个 UI 所属任务，按需轮询触摸 / 键 / IMU | UI 操作集中在该任务；慢存储 / 网络操作提交工作请求 |
| 摄像头预览 | 一个预览任务，采集 -> 提交显示 -> 等待完成 -> 归还 | 优先保证缓冲所有权；忙时丢弃陈旧预览请求，不积累无界帧队列 |
| 音频录放 | 按启用方向设置服务任务，共享 audio 控制面 | 阻塞 I²S 收发或事件等待；有界环形缓冲；溢出计数可观察 |
| 文件写入 | 出现持续录制需求时使用一个写入任务 | 队列满时明确背压或结束录制并报错；不能静默丢录音块 |
| OTA | 使用工作任务或网络已有任务上下文 | 不在 UI / 音频实时回调中下载与写 Flash |

任务间传业务事件或有所有权说明的缓冲描述符，不通过共享可变全局数据传帧。队列必须有固定上限、满队列策略和超时。ISR / DMA 回调只执行该上下文允许的通知操作，不做 I²C、文件 I/O、内存大块申请或 UI 更新。

锁只保护对应共享状态或需要原子性的设备操作；避免持有 I²C / 扩展器锁等待另一个服务、队列或显示完成。持锁不调用任意上层回调，跨模块流程在服务层拆分，防止锁顺序形成环。

## 内存与持久化

- 帧、图片、非实时大缓存显式申请 PSRAM；控制对象、任务栈及实时小缓存优先内部 RAM；应用任务栈按本方案默认使用内部 RAM。检查每次分配失败，计算尺寸时防止整数溢出。
- DMA 缓冲依据具体驱动的能力、对齐和内存要求申请；不把 `MALLOC_CAP_SPIRAM` 自动当作 `MALLOC_CAP_DMA` 的替代。依据 [ESP-IDF 外部 RAM 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html)。
- 运行稳定后减少帧循环中的申请 / 释放，复用固定缓冲。初期 QVGA RGB565 单帧预算 153600 字节，单屏同尺寸；不能未经评估就同时复制多份。
- NVS 保存版本化的小配置，图片 / WAV / 大资源放 SD。当前 Flash 没有文件系统分区，不擅自加入 SPIFFS / LittleFS，也不缩小已确定的两个等大 OTA 槽。
- NVS 初始化异常不可直接擦除全分区；区分版本迁移、数据损坏与空间不足，制定具体恢复方案。文件写失败应保留已有文件，异常中断的录音按可恢复格式处理。
- OTA 写入使用官方 API 与非运行分区；配置双槽不等于已实现升级。启用回滚前同时实现首次启动健康确认，未完成健康确认流程不宣称支持可靠回滚。

## 构建、配置与编码风格

- 继续使用 C 与 ESP-IDF 组件构建。遵循 [ESP-IDF 构建系统](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/build-system.html)：明确声明源文件与依赖；对外头文件需要的依赖用 `REQUIRES`，仅实现需要的依赖用 `PRIV_REQUIRES`。
- 保留 `MINIMAL_BUILD ON`；每个新增组件显式接入依赖链。PSRAM 组件必须留在构建依赖中，不能只保留 sdkconfig 配置而未编译组件。
- 每个组件拥有自己的 `idf_component.yml`（需要外部依赖时）；记录已验证版本范围 / 固定 Git revision，并保留组件管理器生成的 lock。升级依赖作为独立可验证变更，不手工修改 managed_components。
- 固定引脚 / 电平 / 地址放 board；可调功能参数按所属组件放 `Kconfig`，项目级功能组合按需放 main 的 `Kconfig.projbuild`。用户运行设置放 NVS。配置宏统一使用 `CONFIG_SZPI_` 前缀。
- `sdkconfig.defaults` 为持久配置，`sdkconfig` 是生成文件。更新 defaults 同步当前配置并重新生成核对；不能以“defaults 已修改”代替生效验证。
- 文件 / 函数 / 变量用 snake_case，公开符号按组件使用 `szpi_board_`、`szpi_camera_` 等前缀，类型用 `_t`。4 空格缩进，沿用现有大括号独立行风格，常量大写；模块内实现用 static 限定。
- include 使用直接依赖的头文件，不依靠偶然的传递 include；头文件自包含，避免导出内部芯片寄存器和可变全局变量。
- 使用 ESP_LOG 系列，模块固定 TAG；注释解释约束、时序和原因，避免逐句复述代码。未经 profiling 不做影响可读性的微优化。

## 实施顺序与验收

| 阶段 | 实现范围 | 完成条件 |
| --- | --- | --- |
| 1 | board / 安全启动 / runtime / Wi-Fi STA | 基础资源就绪，获得 IP，有限重连与协作停止符合 task_phase_1.md |
| 2 | display / input | 四色、四角、横屏正确；触摸匹配，键和 IMU 可读 |
| 3 | camera + preview | PID=0x2145，QVGA RGB565 显示稳定，帧按时归还 |
| 4 | audio / storage | 双麦克风录音、WAV 写卡、喇叭播放分别验证，之后验证同时收发 |
| 5 | 实际业务 / 网络应用 / OTA | 由产品需求定义；包括故障恢复、升级后健康确认等对应流程 |

每次实现一个可验证能力；构建至少运行 `idf.py build`。修改分区时运行官方生成工具并核对容量 / 对齐 / 等大槽；修改依赖或配置时检查生成配置与组件列表。无需为纯文档或机械接线常量编写镜像测试。

有意义的逻辑测试覆盖扩展器多 bit 更新与写失败、缓冲 acquire/release、初始化失败清理、队列满策略及配置迁移；不以桩函数总返回 ESP_OK 的测试替代硬件验证。需要设备的检查明确记录“未上板”或实际结果，不根据编译通过宣称电气功能通过。硬件烧录 / 擦除遵循用户当前授权范围。

## 维护约定

硬件事实与默认参数维护在 hardware_io.md；结构与长期规则维护在本文件；AGENTS.md 保留必须遵守的摘要。README 提供使用入口。修改接线、资源分配、接口契约或默认参数时同步相关文档；首次引入新产品功能时补充该功能的数据流、失败策略与验证方法，不预先猜测产品路线。
