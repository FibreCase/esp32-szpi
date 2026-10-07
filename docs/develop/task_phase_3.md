# 第三阶段：GC2145 camera 适配与屏幕预览

制定日期：2026-10-07。状态：**代码实现与 IDF 6.1 干净构建已完成；逻辑 / 上板验收待完成**。

依据：[开发规则](../../AGENTS.md)、[硬件接线](../hardware_io.md)、[代码架构](../code_architecture.md)、[第二阶段验证](phase_2_validation.md)。用户要求推进 camera 阶段；此前未完成的逻辑测试和量化验收继续保留，不预填通过。

## 1. 目标与范围

建立 `szpi_camera` 外设适配，验证板载 GC2145 探测、QVGA RGB565 采集、PSRAM 帧缓冲及 LVGL 屏幕预览。通过同一 UI 任务展示真实画面、预览状态与采集错误，保留触摸操作、亮度控制及 Wi-Fi 状态。

本阶段不做拍照文件保存、JPEG 编码、SD、网络视频流、音频、人脸识别或 OTA。先验证单帧，再启用持续预览；不要求摄像头达到 60 FPS，显示约 60Hz 的刷新配置与相机实际采集帧率分别记录。

第二阶段已由用户确认显示与触摸正常、80MHz 刷新流畅。当前不迁移 LVGL 内存池、LCD DMA 缓冲或任务栈；camera 大帧优先 PSRAM，内部资源预算不足时先测量再调整。

## 2. 依赖与兼容性门槛

2026-10-07 查询：组件仓库当前发布包为 [espressif/esp32-camera 2.1.8](https://components.espressif.com/components/espressif/esp32-camera/versions/2.1.8/readme)。计划在 `components/szpi_camera/idf_component.yml` 固定 `==2.1.8`；这只是候选选择，尚未在本项目验证。保留 IDF 6.1、esp32s3、MINIMAL_BUILD、LVGL 9.5.0、Flash / PSRAM 80MHz 与现有双 OTA 分区。

- [x] 固定官方仓库 v2.1.8 tag；本机 registry 存储镜像返回空版本列表，因此以 Git tag 获取源码，组件管理器生成锁文件，不手改 lock 或 managed_components。已复核该版本 [依赖清单](https://github.com/espressif/esp32-camera/blob/v2.1.8/idf_component.yml)及 [CMake](https://github.com/espressif/esp32-camera/blob/v2.1.8/CMakeLists.txt)，IDF 6.1 正常构建。
- [x] 配置新 SCCB I²C 后端，关闭 legacy 选择；代码显式复用 I2C_NUM_0，未引入 GPIO1/2 的第二条总线。IDF 实际构建仍待完成。
- [x] 对照 [SCCB 实现](https://github.com/espressif/esp32-camera/blob/v2.1.8/driver/sccb-ng.c)：复用 board 端口，deinit 移除驱动设备句柄而不删除借用总线；初始化失败路径调用同一 deinit 清理。
- [x] 核对并构建验证 GC2145 RGB565 支持、PID 获取、4 秒采帧等待上限、camera 内部任务与默认 DMA 路径。硬件时序、实际 PID 和图像仍须上板验证。
- [ ] 若候选版本确有兼容故障，记录具体错误并另选可验证版本；禁止重复安装总线、改供应商源码或虚构成功来绕过。

## 3. 接线与初始配置

固定值只进入 board bindings，camera 读取 bindings，不复制另一开发板引脚表。

| 项目 | 初始配置 |
| --- | --- |
| 数据 D0～D7 | GPIO `{16, 18, 8, 17, 15, 6, 4, 9}`，按数据位顺序 |
| VSYNC / HREF / PCLK | GPIO3 / GPIO46 / GPIO7 |
| XCLK | GPIO5，20MHz；LEDC timer0 / channel0 |
| SCCB | board I²C0，GPIO1/2；复用端口，SDA/SCL 配置为 -1，不新建总线 |
| 地址 / sensor PID | 7-bit 0x3C / 0x2145，实际识别结果必须记录 |
| PWDN | PCA9557 IO2，1 掉电 / 0 运行；通过 board 语义接口 |
| pin_pwdn / pin_reset | -1 / -1，不把扩展器 bit 当主控 GPIO，不操作共用 RESET |
| 像素格式 / 分辨率 | RGB565 / QVGA，320×240，期望有效长度 153,600 bytes |
| 帧缓冲 | PSRAM，fb_count=1，GRAB_WHEN_EMPTY |
| 方向 | sensor 保持默认；前置预览在 staging 拷贝时水平镜像，不翻转上下方向 |
| PSRAM DMA | 先核对驱动路径及内部 DMA 预算，建立默认路径基线；再决定是否启用，记录缓存一致性与实测结果 |

启动时 board 保持摄像头掉电；camera 初始化前拉低 PWDN，等待初始 10ms，再由供应商驱动建立时钟与探测。失败后清理本次资源并恢复掉电，保留 LCD CS 和功放状态。保持 LCD SPI2、背光 timer1/channel1，GPIO35/36/37 不可占用。

## 4. 分层与公共契约

| 位置 | 职责 |
| --- | --- |
| szpi_board | camera 固定 bindings、共享 I²C 与 PCA9557 输出影子值 |
| szpi_camera | 驱动生命周期、PID / 帧校验、acquire / release、PWDN、安全错误返回 |
| szpi_app camera / preview service | 跨 camera 与 UI 的协作、预览状态、帧移交与停止 |
| szpi_app UI service | 唯一 LVGL 对象操作入口、图像展示、完成确认、触摸控件 |
| szpi_app runtime | 项目任务表、静态资源创建、启停屏障、状态与监督 |
| szpi_display | 保持 LVGL display 绑定和 LCD DMA 管理，不依赖 camera |

下列为拟定接口要求，不代表已经实现：

- 对外 `esp_err_t`；init、acquire、release、stop / deinit 明确允许线程、超时、重复调用、失败清理及所有权。
- 一个合法 acquire 只能有一次 release。记录 outstanding 帧；拒绝空指针、重复归还、错帧归还或持帧时 deinit。
- 检查 frame 指针、格式、尺寸、长度和时间戳；错误帧必须归还，不能交给 UI 或继续访问。
- 不导出可被任意任务修改的 sensor 指针；方向配置集中在 camera 初始化或受控停止期间。
- 实查 `esp_camera_fb_get()` 的阻塞行为；接口不得声称实现了驱动不支持的任意超时。若其上限超过停止预算，记录实际限制并设计有界停止，禁止强删执行中的任务。

## 5. 任务、队列与内存预算

| 资源 | 初始预算 / 规则 |
| --- | --- |
| szpi_preview | runtime 创建，优先级 4、内部静态栈 4096 bytes、无核绑定；仅预览启用时运行 |
| UI 任务 | 保持现有唯一 szpi_ui，不在 camera 内调用 LVGL |
| 帧移交 queue | 容量 1；所有权随成功入队转移，失败由生产方归还；禁止对帧指针直接 xQueueOverwrite 导致泄漏 |
| 完成确认 | 单消费者 notification / 受控确认；STOP 与帧完成分清通知 bit 或索引，禁止互相覆盖 |
| camera framebuffer | 单帧 PSRAM 153,600 bytes，由驱动拥有，借用期间不得复用 |
| UI 预览 staging buffer | 首版优先独立 PSRAM 153,600 bytes，由 UI 服务拥有；init 时申请，stop 后释放 |
| LCD draw buffer | 保留现有两块内部 DMA buffer，共 51,200 bytes |
| camera 驱动任务 / queue / DMA | 供应商任务例外：实施时登记实际名称、栈、优先级、亲和、容量、销毁路径，不再创建重复采集任务 |

当前上板基线：内部堆空闲 134,983 bytes、历史最低 99,396 bytes；PSRAM 空闲 8,384,412 bytes；UI 栈最低余量 1,736 bytes。此数据来自 camera 接入前的两次日志采样，不是新增 camera 资源的保证。两块完整图像缓冲合计约 300KiB PSRAM，另加供应商资源后重新测量。

初始预览发送节奏上限 15 FPS，可用 CONFIG_SZPI_* 调整；与显示刷新周期分开。等待帧、等待队列或完成确认均需阻塞，无忙等；不在稳定运行中反复 malloc 全帧。

## 6. 预览与缓冲归还顺序

首版选择 PSRAM staging 拷贝方案，优先简化单帧与 UI 生命周期：

1. preview 获取并校验 camera 帧，发送容量 1 的移交消息。保留 token / generation，避免上次启动的迟到通知误认成本次帧。
2. UI 在自己的任务中处理消息。只有上一轮对 staging 的 LVGL 读取及刷新已完成，才可覆盖 staging。
3. UI 将 frame 复制到 staging，水平镜像并完成 RGB565 字节序转换后确认；确认由消费方完成读取，但帧归还仍由明确的 camera 服务执行者调用 release，保证归还线程和所有权一致。
4. UI 使用持久 image descriptor 指向 staging，按 LVGL 9.5 API 失效旧图像缓存、触发重绘；不得在每帧销毁重建控件。相机 RGB565、LVGL 本机格式和 LCD 发送高低字节交换分别核对，不能重复交换。
5. staging 保持有效，直到所有相关 LVGL 读取 / LCD 刷新完成。现有局部 DMA 缓冲继续由 display 完成回调保护。

本方案允许 camera 帧在复制完成后归还，因为后续绘制访问 staging；不能把“入队成功”或“设置 image source”当作读取完成。后续若改为直接借用 camera 帧，必须等所有 LVGL 读取与异步刷新结束后才归还，且先解绑 image source / 缓存；本阶段不先做零拷贝优化。

## 7. 启停、降级与恢复

- 摄像头未安装、PID 错误或初始化失败：显示 camera unavailable 与错误，保留 UI / 触摸 / Wi-Fi；不让驱动深处 ESP_ERROR_CHECK 重启。
- 连续采集失败初始阈值 3 次；记录失败、超时、丢弃数和耗时，达到阈值停止预览并报告故障，不无限自动重启。显式 Start 可进行一次受控重新启动。
- Stop：通知停止 → 停止新采集 → 接收并处理或归还队列帧 → 等在途复制 / 绘制结束 → 解绑图像与缓存 → 确认所有帧归还 → driver deinit → PWDN 掉电 → 确认 STOPPED。
- 先停止 camera 生产，再释放 UI 消费资源；持总线或扩展器锁不等待 UI / 服务确认。
- 首个停止预算暂定 5 秒，实施时按供应商获取帧上限复核。超时报告失败并保留仍被引用的资源，不能强删、假归还、提前释放。
- display 故障或 DMA 未完成时停止新预览，按 staging / DMA 各自所有权保留资源；独立处理 camera 帧归还，不让 camera 无界生产。

## 8. 验证 UI

增加可进入 / 退出的 Camera 预览页，320×240 帧按同一坐标系展示；Start / Stop、返回、状态与 FPS 可覆盖在画面上。切页仅由 UI 执行；保留原验证页和背光设置。

展示真实的采集 FPS、显示完成 FPS、帧尺寸、采集 / 复制 / 刷新耗时、错误数；FPS 使用实际计数 / 时间窗口，不能用配置频率代替。静态页面不应假造持续刷新。

## 9. 实施顺序与验收

| 顺序 | 任务 | 交付与门槛 |
| --- | --- | --- |
| 1 | P3-01 | 已核对源码、固定依赖并通过 IDF 6.1 增量与干净 defaults 构建 |
| 2 | P3-02 | board camera bindings、PWDN 安全顺序、PID=0x2145 已实现并构建 |
| 3 | P3-03 | camera acquire / release、QVGA RGB565 单帧及 PSRAM 帧校验已实现并构建；实测待完成 |
| 4 | P3-04 | runtime preview 任务、容量 1 消息、staging 复制与 LVGL 预览已实现并构建；实测待完成 |
| 5 | P3-05 | 状态、Start / Stop、切页、降级与有限恢复已实现并构建；逻辑和上板验证待完成 |
| 6 | P3-06 | 失败路径逻辑测试、上板颜色 / 方向校准、内存与长期验证 |

### 构建与逻辑测试

- [x] 代码 / 依赖 / 配置修改后执行 IDF 6.1 `idf.py build`；使用 `/tmp/szpi-phase3-clean` 与新 sdkconfig 核对 defaults。lock 为 esp32-camera commit `b0556a78e13d42974aae19116f8a1e14c29b0f4d`、esp_jpeg 1.3.1、LVGL 9.5.0、IDF 6.1.0；当前镜像 0x131890 bytes，OTA 槽 0x7f0000 bytes，余量 0x6be770 bytes（约 85%）。
- [ ] 测试 init 各阶段失败逆序清理、共享 I²C 不删除、扩展器仅改变 PWDN bit。
- [ ] 测试尺寸 / 格式 / 长度非法帧、重复 release、持帧 deinit、queue 满 / STOP 排空和迟到 token。
- [ ] 测试复制完成前不归还帧、staging 绘制期间不覆盖、DMA 超时不释放缓冲。
- [ ] 测试启动失败、连续采集错误、camera 缺失及 display 故障时的真实降级状态。

### 上板验收

- [ ] PID=0x2145，连续采集至少 100 帧，尺寸 / 格式 / 长度合法，RGB565 色序与字节序正确。
- [ ] 实拍文字或方向标记，确认上下 / 左右与预览一致；触摸、亮度、Start / Stop 和返回可用。
- [ ] Wi-Fi 已连接和断线恢复期间预览可用，记录实际 FPS、最长采集 / 复制 / 刷新延迟、超时及错误数。
- [ ] Start / Stop 和切页各 10 次，无重复总线 / 任务安装；帧借出归还平衡，预热后堆和最大连续块无持续下降。
- [ ] 连续预览 30 分钟，无异常重启、watchdog、堆损坏或撕裂；记录内部 / PSRAM 最低堆、DMA 最大连续块、UI / preview / 供应商任务栈余量。
- [ ] 停止后 camera 掉电、背光设置保持、LCD / 触摸仍工作，下一次 Start 可恢复。

实施后创建 `docs/develop/phase_3_validation.md`，记录实际版本、命令、错误、测量与未验证项，同步 README / AGENTS / 架构 / hardware_io。构建成功不等于上板通过；完整验收前不标阶段完成。
