# 第四阶段实现与验证记录

更新日期：2026-10-08。状态：**初版代码实现；软件构建通过；逻辑与实物验收待完成**。

## 已实现

- 新增 `szpi_audio`、`szpi_storage`，板级 bindings 增加 ES8311 / ES7210 地址、I²S 和 SDMMC 引脚；audio 借用 board 已安装的新 I²C master bus，未重复安装控制器。
- `esp_codec_dev` 固定为 1.6.2。audio 组件创建 I²S_NUM_0 master TX STD / RX TDM 通道，配置 48kHz、16bit、MCLK×256，并提供 codec 初始化、低音量输出、采集增益及有界 PCM 读写接口。
- 根据实板日志修正 codec 接入：board bindings 使用 7-bit 地址，`esp_codec_dev` 1.6.2 的 I²C 适配器接口使用 8-bit 地址并在内部右移，因此 audio 初始化时先将 0x18 / 0x41 左移后交给库；TX / RX codec 共用同时包含两个 I²S handle 的 data interface，与该依赖的全双工例程一致。I²S 初始化失败现在会标出失败阶段及内部堆余量，便于区分 DMA 分配失败与 codec 总线故障。
- 根据实板日志修正 SD 路径及清理：直接调用 FatFs 的 `f_mkdir` 使用 `0:/recordings` 逻辑盘路径；SDMMC 释放统一调用一次 `sdmmc_host_deinit()`，避免先删最后一个 slot 后再次删除同一 controller。录音 / 回放 UI 在 FAT32 就绪前禁用，并在音频服务忙时禁用。
- storage 分阶段初始化 SDMMC 1-bit、diskio、VFS 与 FatFs；普通挂载不自动格式化，挂载后要求 `FS_FAT32`。状态暴露容量、空闲空间、generation 与真实错误。
- FAT32 格式化由 UI 两步操作确认，并在 storage runtime task 串行执行。实现检查当前 generation、打开文件互斥、MBR/FAT32 容量边界，使用 `f_fdisk` 单分区和显式 `FM_FAT32`、双 FAT、32KiB cluster；完成后重新挂载并进行小文件写读校验。格式化行为尚未对实卡执行。
- AUDIO 与 SD CARD 验证入口分成独立页面；AUDIO 页面提供 440Hz 测试音、双麦声级采样、停止、WAV 录音 / 最近录音回放，以及麦克风增益（0–36 dB，默认 18 dB）和扬声器音量（0–100%，默认 70%）滑条。调节请求由 audio runtime task 串行应用到正在采集或播放的 codec，也作为后续录音、测试音与 WAV 回放的设置。输入增益提高及两条滑条尚待上板校准。测试音峰值为 12000，并记录 PA 首次开启和 PCM 写错误。SD CARD 页面提供状态 / 容量、重试和双步格式化确认。文件 I/O、mkfs 和阻塞 PCM 操作由 runtime 管理的服务任务处理。
- 录音生成 `recordings/rec_<uptime>.part`，写占位 PCM WAV 头；录制期间累计更新峰值与 RMS；正常停止后排空当前块、回填 RIFF / data 长度、sync、close 并改名 `.wav`。异常时保留 `.part`。回放解析 RIFF chunk（最多扫描 32 个）、fmt/data、填充字节与参数，支持 48kHz 16bit mono/stereo PCM，mono 复制为双槽输出。
- 按用户指定的[嘉立创基础外设例程总览](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/base-peripheral-examples.html)核对，并参考其[SD 卡示例](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/sd-card.html)、[ES7210 输入示例](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/audio-input-es7210.html)和[ES8311 输出示例](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/audio-output-es8311.html)。SDMMC 示例提供 1-bit slot、20MHz、引脚绑定和卡信息打印的参考；ES7210 示例采用 48kHz、16-bit、TDM slot0/slot1 与 MCLK×256。实现保留这些接线与初始参数，并复用项目新 I²C。教程里 mount failed 时自动 format 的演示配置未照搬，因本任务要求格式化需显式确认。

## 软件验证

| 检查 | 结果 |
| --- | --- |
| ESP-IDF | 6.1，target `esp32s3` |
| Codec 依赖 | `espressif/esp_codec_dev` 1.6.2，已写入 manifest 与生成的 `dependencies.lock` |
| 构建 | `source /home/fibre/.espressif/v6.1/esp-idf/export.sh && idf.py build` 通过 |
| 干净 defaults | `idf.py -B /tmp/szpi-p4-clean-default -D SDKCONFIG=/tmp/szpi-p4-clean-default/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build` 通过；临时目录生成独立 sdkconfig |
| app 镜像 | 最新干净 defaults 构建为 0x15A9D0 bytes；7.9375MiB OTA app 槽余量约 83%；未改分区表 |
| WAV 逻辑测试 | `cc -std=c11 -Wall -Wextra -Werror tests/test_szpi_wav.c components/szpi_app/src/szpi_wav.c -o /tmp/test_szpi_wav` 后运行通过；覆盖头部生成、RIFF 奇数填充块、mono/stereo 与损坏 / 不支持格式 |
| 其他专项逻辑测试 | FAT mount / format 生命周期、队列所有权、初始化失败清理等 host 测试尚未实现 |

构建过程最初因沙箱无法写入 Component Manager 外部缓存而失败；授权后依赖解析完成并成功构建。该权限只用于构建依赖，不涉及烧录、擦除或设备操作。

## 仍待完成与风险

- WAV 录音目前在 audio RX 服务任务中同步写 SD；尚未实现任务计划中的 16×3840-byte PSRAM PCM pool、容量 16 的块队列、独立 storage writer、队列峰值与可验证的 overrun stop 策略。因此尚不能据此保证持续录音无丢块；长录音、慢卡和相机并行需要先完成分块队列实现并做负载测试。
- 当前 runtime 只有一个 `szpi_audio_rx` 服务任务；TX 由该服务同步驱动，尚未拆出独立 `audio_tx` 任务。独立方向统计完整度、TX underrun 和 RX DMA overrun 尚未建立。
- `.part` 异常文件保留但没有显式恢复 / 校正流程；本次启动仅可播放成功收尾并记入 RAM 的最近 `.wav`，没有扫描 / 选择旧录音列表。
- generation 绑定当前 storage 初始化周期；板上 SD 座没有 CD pin，尚未实现可验证的物理换卡身份复核。格式化时依靠卡命令错误停止，拔插情形未验证。
- TX STD 与 RX TDM 共用 GPIO / MCLK 配置虽通过编译，BCLK/WS 真实波形、codec slot 映射、同时启用时的 ESP32-S3 I²S 行为尚未上板确认。编译不证明 TX/RX 时序兼容。
- 无逻辑测试覆盖初始化失败逆序清理、重复 init、无卡、FAT16/exFAT、磁盘满、短读写、过期 generation、文件活动时格式化、format 后重新挂载及 `.part` 恢复。
- 未烧录固件、未连接 / 操作开发板、未对任何 SD 卡执行格式化；麦克风映射、功放输出、录音回放、同时录放、UI 并行负载与 30 分钟稳定性均待验证。

## 板上验收记录

用户提供的日志已确认 ES8311 / ES7210 能成功打开，SD 最终挂载为 FAT32；日志同时暴露过时序内的录音请求，以及错误传给 FatFs 的 `recordings` 目录路径和重复 deinit。以上软件问题已修复并通过 IDF 构建，但新固件尚未再次上板，故录卡 / 回放仍未确认。按任务要求，只有用户明确指定并授权测试卡后，才进行破坏性格式化；之后逐项记录 FAT32 PC 识别、WAV 时长 / 双声道、长时间吞吐、资源余量和并行 camera/Wi-Fi 负载。本节保持未通过，不能用构建结果代替。
