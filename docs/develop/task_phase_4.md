# 第四阶段：audio / storage 适配与 FAT32 SD 卡

制定日期：2026-10-08。状态：**初版代码已实现并通过 IDF 6.1 构建；逻辑与上板验收未完成**。

依据：[开发规则](../../AGENTS.md)、[硬件接线](../hardware_io.md)、[代码架构](../code_architecture.md)。继续使用 ESP-IDF 6.1 / esp32s3、16MiB Flash、8MiB PSRAM、现有分区和已适配的显示 / 输入 / camera；此前未完成的量化验收继续保留。

## 1. 目标与范围

实现 `szpi_audio`、`szpi_storage`：双麦克风采集、喇叭播放、SDMMC 1-bit SD 卡、FAT32 文件访问与显式格式化。通过应用服务完成 PCM WAV 录音写卡、文件回放和兼容时序的同时录放，提供 UI 验证入口。

SD 文件系统必须是 FAT32，不将 FAT16 或 exFAT 挂载成功记为通过。具备已格式化卡重新格式化，以及空白 / 不支持文件系统卡初始化后格式化的能力。格式化目标仅为 SD 卡，不能修改内部 Flash 分区、NVS 或 OTA。

本阶段不做 MP3 / AAC 解码、AEC、第三路模拟回采、网络音频、拍照保存、完整文件管理器或自动热插拔。先分别验证 ADC、DAC 和文件读写，再验证录音 / 回放，最后验证同时录放及其它服务并行负载。

当前实现范围和未完成项见[第四阶段验证记录](phase_4_validation.md)。该记录优先于下文尚未勾选的验收条目；初版构建通过不表示板上音频 / SD 已可用。

## 2. 资料与候选依赖

- 音频参数参考 [立创 ES7210 输入例程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/audio-input-es7210.html) 和 [ES8311 输出例程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/audio-output-es8311.html)，接线仍以 hardware_io 为准。
- 候选 Codec 依赖：[esp_codec_dev 1.6.2](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2/readme)，支持 ES7210 / ES8311；尚未在本项目验证。实施时核对 IDF 6.1、新 I²C 借用、I²S 句柄接入和关闭路径，验证后在 audio manifest 固定版本，由组件管理器生成锁文件。
- SD 使用 IDF `sdmmc` / `esp_driver_sdmmc` / FatFs / VFS；依据 [IDF FAT 文件系统文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/storage/fatfs.html) 及本机 IDF 6.1 源码。
- 所有外设控制复用 board 新 I²C，不安装 legacy 总线，不手改 managed_components。不合适的供应商接口在 audio 内用最小受控适配解决，记录原因，不建立通用 HAL。

## 3. 接线与初始参数

固定 GPIO、地址和 PCA9557 bit 只扩展 board bindings；可调参数使用 CONFIG_SZPI_*。

| 项目 | 初始配置 |
| --- | --- |
| 音频控制 | ES7210 0x41、ES8311 0x18；借用 board I²C0 / GPIO1、2 |
| I²S | I2S_NUM_0；MCLK=38、BCLK=14、WS=13、TX DOUT=45、RX DIN=12 |
| 采样 | 48kHz、16bit，双麦克风 MIC1 / MIC2；MCLK 12.288MHz |
| RX | 参考官方 TDM slot0 / slot1；实际槽序和总帧时序须验证 |
| TX | 两槽 Philips，单声道样本复制左右槽；ES8311 DAC 槽需核实 |
| 共享时钟 | 主控 master，单一 MCLK/BCLK/WS 源；TX/RX 参数统一管理 |
| 功放 | NS4150B；PCA9557 IO1 高开启、低关闭，走 board 语义接口 |
| SDMMC | 1-bit，CLK=47、CMD=48、D0=21；无 CD / WP GPIO |
| SD 时钟 | 初始 20MHz；卡初始化频率遵守 IDF 流程，不配置 4-bit 或 UHS |
| 存储 | FAT32；挂载 `/sdcard`；首版测试卡建议 8–32GB，64GB 及以上另测 |
| 文件格式 | PCM WAV，48kHz / signed 16bit，录音双通道；支持同参数单 / 双通道播放 |

ES8311 的 ADC 输出未接主控，不能充当录音输入。ES7210 第三路是 DAC 模拟回采，不是第三颗麦克风，本阶段不启用。GPIO45 是启动配置脚，不变更启动电路。

供应商必须复用由 audio 统一创建的 I²S 通道；不能各自安装控制器 / 时钟。核对 STD TX 与 TDM RX 的 WS 波形、槽位宽度、每帧总时钟和 IDF 同时收发支持，先验证一致再启用同时录放。不能仅凭均为 48kHz / 16bit 宣称兼容；若初始组合不兼容，统一调整可验证的帧格式并记录，不添加第二个主时钟源。

## 4. 结构与接口

| 位置 | 职责 |
| --- | --- |
| szpi_board | audio / SD 固定 bindings、共享 I²C、功放安全状态 |
| szpi_audio | Codec、统一 I²S、增益 / 音量、PCM 有界读写与生命周期 |
| szpi_storage | SD 卡、FatFs / VFS 注册、挂载 / 卸载 / 格式化、文件与容量接口 |
| szpi_app audio / recorder service | 录放控制、音频块移交、WAV、跨 audio / storage 的协作 |
| szpi_app storage service | 有界请求处理、持续文件 I/O、格式化执行与结果通知 |
| szpi_app UI | Audio / Storage 验证页及状态，所有 LVGL 操作仍在 UI 任务 |
| szpi_app runtime | 项目任务表、静态资源、启停和监督；main 仍只启动装配 |

公共 API 返回 esp_err_t，注明超时、线程安全、允许调用状态、重复调用、数据长度和所有权。audio 不依赖 storage，storage 不编排 audio；协作属于 app。FILE / FIL 不随意暴露给上层长期持有，storage 追踪所有打开对象，以保证卸载 / 格式化互斥。

音量与采集增益有明确范围；播放初始低音量、测试音幅度受限。启动 PA 默认关闭，Codec / 时钟 / 有效 PCM 及输出静音解除准备好后才开启；停止播放先静音关闭 PA，再停止数据流和时钟，失败也不能遗留 PA 开启。

## 5. FAT32 挂载与文件契约

- [ ] 主机 / slot / card / diskio / VFS / FATFS 各阶段登记所有者和清理顺序；重复 mount 不重复注册资源。
- [ ] 正常挂载 `format_if_mount_failed=false`。无卡、损坏、FAT16 / exFAT 或不支持的分区返回真实状态，不自动清卡。
- [ ] 挂载后核实实际 FAT 类型为 FAT32；不以 Kconfig 开启 FatFs 支持或 API 成功替代类型检查。
- [ ] 提供卡信息、容量 / 空闲空间、目录、创建 / 读 / 写 / seek / sync / close 所需接口；首版目录 `/sdcard/recordings`。
- [ ] 初始最多 4 个打开文件，请求队列固定容量；序列化写入和格式化，不在 UI / ISR 执行文件 I/O。
- [ ] 路径限定在 SD 根目录，拒绝逃逸、超长、非法及不支持路径。命名不覆盖已有录音，文件名 / 时间戳无需依赖网络时间才能工作。
- [ ] 短读写、磁盘满、写保护及卡拔出均向上返回；记录已写字节，不把短写当成功。无 CD 引脚，靠操作错误进入 fault，显式重新挂载，不做无限检测 / 重试。
- [ ] 卸载拒绝活动文件和在途 I/O，或由服务先有序收尾；成功 close / sync 后才确认完成。数据异常不以自动格式化恢复。
- [ ] FAT32 单文件上限与 RIFF 32bit 长度都要检查；录音达到限制前有序结束或按明确分段策略处理，不整数溢出。

## 6. 必须具备的 FAT32 格式化能力

### 功能与介质约束

1. 默认提供快速格式化，删除 SD 原有文件系统与数据；不声称安全擦除或不可恢复。
2. 初始明确支持 SD 单 MBR 主分区布局，格式化后建立占用可用卡空间的 FAT32 卷；显示会清除整张卡。原有多分区 / exFAT 等卡在确认后按该布局重新建立，不保留隐藏分区。
3. 实施前验证容量、扇区、FAT32 簇数及簇大小可行性；过小或不支持容量必须拒绝，不静默降级 FAT16。簇大小按容量选择，初始吞吐验证可采用 32KiB；保持两份 FAT。
4. 调用 FatFs `f_mkfs` 时显式选择 `FM_FAT32`，按已确定分区布局使用受控 `f_fdisk` / mkfs 流程。IDF 6.1 通用格式化 helper 内部为 `FM_ANY`，不能不经核对就用于满足强制 FAT32 要求。
5. 覆盖已挂载 FAT32 和“卡初始化成功但文件系统不可挂载”两种入口。检查 IDF helper 的 context / diskio 注册条件；必要时由 storage 分离卡初始化、diskio 注册与文件系统挂载，不能要求先成功挂载 FAT32 才能格式化空白 / exFAT 卡。

### 用户流程与互斥

- UI 显示所选卡容量 / 标识和“清除整张 SD 卡全部数据”，由独立确认按钮提交 Format FAT32；普通 mount、启动或 BOOT 短按不能触发格式化。这里是产品功能的操作确认，不代表本阶段获准实际格式化用户设备。
- 确认绑定当前挂载 generation / 卡信息；换卡、重新初始化或超时后旧确认失效。无 CD 引脚不能保证电气检测换卡，执行前复核卡可访问性及可取得的身份，出现错误就停止。
- 状态进入 FORMAT_PENDING，拒绝新的文件请求，由 app 停录放并收尾；关闭全部文件、确认无在途 I/O，独占卷后进入 FORMATTING。未完成收尾不得开始破坏性写入。
- 格式化由 runtime 管理的 storage 服务执行，UI 显示忙状态，禁止重复提交；只有开始破坏性写入前允许取消，写入开始后不能假称可安全取消或强删任务。
- 大容量格式化不伪造 3 / 5 秒完成上限；实施时测量并定义合理预算。超时调用方可获知 BUSY / timeout，底层执行未结束不能释放 card / 工作缓冲或开放新 I/O。
- 成功后重新挂载、验证 FAT32、读回容量并做独立小文件写入 / sync / 读取校验 / 删除；全部通过才进入 READY。失败保留错误与卡状态，不报告成功或无界重试。
- 不在开发验证中擅自格式化实卡；破坏性验收只对用户明确指定并授权的测试 SD 卡执行。

## 7. 录音、回放与同时收发

先用低幅度固定 PCM 测试音验证 DAC / PA，再验证双麦克风声级与通道映射，然后录卡回放。Audio Test 的麦克风检查录制 5 秒双通道 PCM 到 PSRAM，再从该缓冲播放；不足 5 秒或录音失败时不播放。首版提供扬声器音量与麦克风增益调节、Audio Test 页面、Record / Stop、选择本项目录音 / Play / Stop 及 SD 状态。扬声器默认 50%，麦克风默认 100%（0–36dB），两个百分比均持久化到 NVS；滑动时实时应用，松开或离开调节页时保存。

- 录音块按 stereo frame 对齐；48kHz / 16bit / 双通道为 192,000 bytes/s。DMA 小缓冲内部 RAM，文件队列 / 较大积压缓冲显式 PSRAM。
- 录音先建立唯一 `.part` 文件及合法占位 WAV 头，再允许 RX 生产；停止后排空已接收块、更新 RIFF / data 长度、sync / close，全部成功才更名 `.wav`。
- 若录音中断，保留 `.part` 与错误；后续显式恢复根据实际文件长度校正，丢弃不完整尾部 frame，不能把未完成文件伪装正常录音。FAT32 无事务保证，掉电恢复只能尽力，不声称绝对安全。
- 录音队列满 / SD 连续慢写必须可观察；初始策略为停止录音并报告 overrun，收尾已有块，不静默丢块或阻塞到 I²S 无声溢出。
- 回放解析 RIFF chunk（含填充），不假设所有 WAV 数据固定在第 44 字节；仅接受明确支持的 PCM 参数，拒绝截断、长度溢出、压缩或其它采样率，暂不重采样。
- 单声道复制到左右槽；双通道到单声道 DAC 选择明确的混音 / 槽策略并避免溢出。TX 短写循环有截止时间；队列 underrun 输出受控静音、累计次数，连续异常停止并关闭 PA。
- 同时录放只对时序已确认的格式开放，先验证测试音播放 + MIC1/MIC2 采集，再验证播放文件 + 另一独立文件录音。不读写同一活动录音文件，不启用回采 / AEC。

## 8. 任务、队列与停止预算

以下均为实施预算起点，按测量调整，并在 runtime 集中登记；不用空任务提前占资源。

| 资源 | 初始预算 / 所有权 |
| --- | --- |
| audio_rx | runtime 创建，优先级 8、内部静态栈 6144 bytes、无核绑定 |
| audio_tx | runtime 创建，优先级 8、内部静态栈 6144 bytes、无核绑定 |
| storage service | runtime 创建，优先级 6、内部静态栈 4096 bytes、无核绑定；持续 I/O / 格式化复用同一服务 |
| RX / TX PCM 池 | 每方向 16 块 × 3840 bytes（20ms PCM），各 61,440 bytes PSRAM；控制描述符内部 RAM |
| PCM 队列 | 每方向容量 16；成功移交才转移块所有权，消费后归还池；STOP 排空显式回收 |
| 文件请求队列 | 容量 8；命令按值或持久请求对象，超时后不持有调用方栈地址 |
| I²S DMA / SD staging | 按实际驱动对齐与能力申请内部 RAM，初始量在驱动核对后登记，失败逆序清理 |
| 格式化工作缓冲 | storage 拥有，容量 / 对齐按 FatFs 和 SDMMC 核对；开始前申请，执行完成后释放 |

16 块仅提供约 320ms 积压余量，不保证任意 SD 卡持续速度。登记队列峰值、PCM overrun / underrun、文件最长写耗时、吞吐、内部 / PSRAM 最低堆与最大连续 DMA 块、任务栈余量。不要根据旧 camera 接入前内存日志直接认定预算充足。

普通 Stop 初始目标 5 秒：停止新 RX / TX 生产、关闭 PA、收尾在途 I²S 和文件请求、归还所有 PCM 块、sync / close、确认停止，最后关闭 Codec / 通道。I²S 与文件操作使用实际可实现的有界等待；超时报告并保留仍被引用资源，不强删任务。格式化中的 Stop 遵守不可中途强制释放规则。

## 9. 状态与降级

audio 分方向状态，storage 区分 UNINITIALIZED / NO_CARD / CARD_READY_NO_FS / READY / BUSY / FORMATTING / FAULT。无卡允许测试音与麦克风验证；音频故障不影响 SD 管理；SD 故障停止依赖文件的录放，保留 UI / touch / IMU / camera / Wi-Fi。

重试只有有限次数或显式操作；格式化不是通用恢复手段。UI 不执行 Codec I²C、音频阻塞读写、文件扫描或 mkfs，只发送有界请求和读取状态快照。

## 10. 实施顺序

| 顺序 | 任务 | 交付与门槛 |
| --- | --- | --- |
| 1 | P4-01 | Codec / SD / FatFs 版本与共享时钟兼容核对，board bindings、接口与资源表 |
| 2 | P4-02 | SD 卡初始化、FAT32 挂载、文件读写 / sync、容量及错误降级 |
| 3 | P4-03 | FAT32 强制格式化、不可挂载卡入口、确认流程与独占生命周期 |
| 4 | P4-04 | audio 控制、ES8311 测试音、PA 安全状态、ES7210 双麦采集 |
| 5 | P4-05 | runtime 录放 / storage 服务、PCM 池、WAV 录卡与回放 |
| 6 | P4-06 | 同时收发、UI 管理页、并行负载与故障恢复 |
| 7 | P4-07 | 逻辑测试、授权测试卡格式化、上板校准及长期验收 |

## 11. 验收

### 构建与逻辑测试

- [ ] 每次代码 / 配置 / 依赖修改执行 idf.py build，核对干净 defaults、lock、镜像及 OTA 余量；不修改内部 Flash 分区。
- [ ] init 各失败点逆序清理、重复启停、新 I²C 复用、I²S 单时钟源、PA bit 保留其它扩展器输出。
- [ ] PCM 移交 / 归还、队列满与 STOP 排空、TX 短写 / underrun、RX overrun，不能漏归还或提前复用。
- [ ] WAV chunk / 参数 / 长度边界、短读写、容量耗尽、`.part` 恢复与 RIFF / FAT32 文件上限。
- [ ] mount 失败无自动格式化，FAT32 类型核实；格式化未确认、活动文件、重复提交、过期 generation、取消与超时状态均符合规则。
- [ ] 空白 / exFAT 卡初始化但 mount 失败仍可进入明确授权格式化；不支持容量拒绝而不回退其它 FAT 类型。

### 上板验收

- [ ] 授权测试卡正常 mount、读写校验、容量显示；分别格式化原 FAT32 与不可挂载卡，PC 与设备均确认 FAT32，旧数据不再作为有效文件出现。
- [ ] 未确认 / 有在途文件时不执行格式化；执行期间 UI 有响应，重复按钮无重复写卡；无卡 / 卡拔出时真实报错。
- [ ] PA 启动及停止状态正确，测试音、音量、双麦独立通道、采样率与时钟槽映射实测记录。
- [ ] 录音至少 60 秒并在 PC 验证 WAV 参数、时长、双通道与声音；本机回放无明显断音，实际耗时与字节数匹配。
- [ ] 同时录放至少 10 分钟，无静默丢块，记录 overrun / underrun 与 SD 最长 I/O；不足时不能记通过。
- [ ] 连续运行 30 分钟及各 10 次录放 / mount-unmount，不出现异常重启、堆损坏、持续堆下降或未归还 PCM。
- [ ] camera 预览、Wi-Fi、触摸 / IMU / BOOT 并行下实测帧率、音频连续性和存储吞吐，明确支持负载组合。

实施后创建 `docs/develop/phase_4_validation.md`，记录实际版本、命令、授权测试卡、格式化结果、音频校准、测量及未验证项；同步 README / AGENTS / 架构 / hardware_io。任务制定、构建与实物通过分别标记，不预填成功。
