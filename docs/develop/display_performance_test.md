# 显示帧率与撕裂测试

入口：主页左划进入 Settings，点击 Display，再点击 FPS / Tearing。右划返回 Display，测试自动停止；再次进入重新开始采样。模拟器右方向键对应左划、左方向键对应右划。

测试页以 16ms LVGL timer 更新移动黑白条纹，并主动标记整个 320×240 画布需要刷新。标题栏和指标栏覆盖在画布上，以保持文字可读。图案按时间移动，降低帧率时不会降低设定运动速度。看竖直边界是否在不同高度错位；可用手机慢动作拍摄辅助观察。

## 板上指标

测试在进入动画结束后开始，退出动画开始时停止；首个统计窗口约一秒。显示链路使用 80MHz、40 行双 DMA 缓冲与单个在途传输，测试不改变这些配置。早期基线使用 40 行双缓冲。

| 指标 | 定义 |
| --- | --- |
| FPS | 统计窗口内完成的 LVGL 刷新周期数 / 窗口时间；只在最后一块像素 DMA 完成后计一帧，不把分块次数当帧数 |
| TX 平均 / 最大 | 同一刷新周期第一块提交前，到最后一块 DMA 完成的耗时；包括命令与分块间等待，不包括第一块提交前的绘制与字节交换 |
| LVGL | `LV_EVENT_RENDER_START` 到 `LV_EVENT_RENDER_READY` 的平均耗时，包括绘制、提交和刷新过程中的 DMA 等待；它不是纯 CPU 绘制时间，也不能和 TX 简单相加 |
| Gap | 每帧内从上一块 DMA 完成到下一块提交前的空档总和的平均值；不包含两帧之间的空闲时间 |
| gap max（串口） | 窗口内最大的单次分块空档 |
| pixels/frame（串口） | 每个已完成刷新周期的平均发送像素数；整屏更新应接近 76,800，用于识别局部更新与整屏更新的区别 |

ISR 仅记录完成时间并发布原有 DMA semaphore，UI 任务取得 semaphore 后处理统计；同一块缓冲仍要等 DMA 完成才能归还。统计状态属于 szpi_display，应用通过只在 UI 任务调用的接口获取快照，再传给共享 UI。

串口每秒输出一次 `szpi_display: display test: ...`，包含 FPS、TX 平均/最大、LVGL、Gap、单次 Gap 最大值和每帧像素数。记录至少 10 秒的稳定运行数据；初次采样和退出动画不用于对比优化效果。串口打印和屏幕指标也有少量开销，各方案比较时保持同一测试条件。

这些指标测量主控提交画面的完成情况，不测量面板扫描频率，也不会自动判断撕裂。FPS 较高仍可能出现撕裂；当前接线没有 TE 帧同步信号。需要结合图案观察判断优化效果。

## 首轮优化与对比基线

用户提供的板上日志：整屏 76,800 pixels/frame，稳定约 32 FPS，TX 平均约 21.3ms，LVGL 约 23.3ms，Gap 约 4.3ms/帧；肉眼观察条纹上下错位。

首轮优化使用 LVGL 的 RGB565 批量字节交换实现，利用 32 位批量操作替代逐像素字节读写。测试图案更新后将显示刷新 timer 标记 ready，避免动画 timer 与刷新 timer 执行顺序导致更新等待另一个刷新周期。仍由唯一 UI 任务执行，不增加任务、缓冲或 DMA 在途数量。PSRAM 暂不增加帧缓冲，先用相同内存预算比较这一轮收益。

重新烧录后记录稳定运行至少 10 秒的同一测试日志，比较 FPS、TX、Gap 和条纹错位。构建通过不能代表性能提升或撕裂消失；优化后板上数据待补充。

首轮板上复测：稳定约 34.2 FPS，TX 约 19.7ms，LVGL 约 21.2ms，Gap 约 2.8ms/帧，内部 RAM 和 PSRAM 日志用量未增加。

第二轮将固件编译优化从默认调试 `-Og` 切换为性能 `-O2`（`CONFIG_COMPILER_OPTIMIZATION_PERF=y`），保留断言与错误检查。该选项影响整个应用及依赖，可能增加固件代码大小、降低源码单步调试的直观性；不改变任务、接线或缓冲预算。板上性能和栈余量须用第二轮日志确认，比较基线为 34.2 FPS / TX 19.7ms / Gap 2.8ms。模拟器编译配置不随固件设置改变。

第二轮 IDF 6.1 在新目录 `build-perf` 完整构建通过，应用镜像 0x161bd0 字节，OTA 槽剩余约 83%。旧 `build` 目录重新配置遇到依赖管理器缓存 KeyError；本轮烧录使用 `idf.py -B build-perf flash monitor`，避免使用旧目录的首轮固件。未执行烧录，板上复测待完成。

## 模拟器与验证边界

模拟器共用图案和导航，但显示 `Desktop preview / Hardware stats on device`，不模拟 SPI 或 DMA 指标。主机检查覆盖进入/退出事件、条纹移动、指标格式、桌面提示和销毁后重建。固件需要上板运行后才能获得真实测量结果；本次未烧录或声称完成撕裂验收。

资源：一个 LVGL timer 由 szpi_ui 创建、在测试页进入时恢复、退出时暂停、销毁 UI 时删除；不创建 FreeRTOS 任务，不增加 DMA 或 PSRAM 图像缓冲。统计字段为 szpi_display 的固定模块状态，使用原有完成 semaphore 发布完成时间。

## 第三轮：减少整屏分块

第二轮板上稳定约 37.2 FPS，TX 约 18.5ms，LVGL 约 19.2ms，Gap 约 1.7ms/帧，用户仍观察到撕裂。内部空闲堆当前 86,867B、历史最低 51,520B，UI 栈余量 832B。

第三轮将两块内部 DMA 缓冲各从 40 行增至 60 行，整屏从 6 块减至 4 块；总缓冲从 51,200B 增至 76,800B。SPI 最大传输大小随缓冲宏同步变化。增加 25,600B 内部 RAM，按旧日志推算最低空闲堆约 25,920B；这只是预算估算，不代表新固件实际余量，须检查 runtime 日志与初始化结果。分配失败沿用 init 的逆序清理与显示故障上报。

保持单个在途 DMA、双缓冲归还规则、80MHz SPI、任务与刷新周期。PSRAM 用量不增加。预期减少分块空档，但整帧写入仍与面板扫描异步，不能保证消除撕裂。板上需同时比较 FPS、TX、Gap、最低堆、栈余量和肉眼错位；第三轮未烧录。

第三轮 IDF 6.1 `build-perf` 完整构建通过，应用镜像 0x161bd0 字节，分区容量检查通过。

第三轮板上初始化返回 ESP_ERR_NO_MEM，60 行缓冲方案撤回，恢复已验证的 40 行 / 51,200B 总预算。此前用总空闲堆估算 DMA 预算不足以判断可分配性：DMA 需要满足能力与连续块条件，SPI 最大传输变化也可能增加驱动资源。当前日志无法确定具体失败分配位置。保留前两轮优化。失败清理中的 LEDC timer 先 pause 再 deconfigure，符合 IDF 删除运行 timer 的要求；本次修复待板上重验。

用户要求重试 60 行 DMA：当前再次启用 60 行缓冲，保持 LEDC 清理修复；增加初始化前 DMA 可用总量、最大连续块和每块申请大小日志，区分 buffer 1、buffer 2 和 LVGL display 分配失败。失败清理前再次打印 DMA 堆状态。不自动回退为 40 行，失败仍上报；上板结果待确认。

## PSRAM 内存布局调整

60 行重试日志确认第二块 38,400B 内部 DMA 缓冲分配失败：申请前 DMA free=132,471B / largest=65,536B；第一块成功后 free=93,555B / largest=31,744B。

启用 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`，在顶层 IDF CMake 仅对 LVGL target 注入 `esp_attr.h` 与 `LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR`。LVGL 9.5 此属性用于内置 allocator 的静态 64KiB pool，将其放入 PSRAM BSS，生命周期随固件；不引入动态 pool 申请与释放，不修改 managed_components。页面对象等由 LVGL 管理，LCD 的两块 38,400B DMA 缓冲仍显式申请内部 RAM，不让 SPI DMA 直接访问 PSRAM。共享 UI 与模拟器不受此 IDF target 设置影响。

预计内部静态 RAM 减少约 64KiB，PSRAM 静态使用相应增加；实际连续 DMA 空间与速度须上板确认。PSRAM 访问可能增加绘制开销，因此应比较初始化 DMA 日志、FPS/TX/Gap 和 runtime 内存/栈。

PSRAM 布局构建通过（IDF 6.1，应用 0x161f70 字节），模拟器构建通过。链接 map 确认 `lv_mem_core_builtin.c.obj` 的 0x10000 字节池位于 `.ext_ram.bss` 的 0x3c160000，已离开内部 RAM。启用外部 BSS 后 IDF 也按自身链接规则将部分 lwIP BSS 放入 PSRAM；本次外部 BSS 总大小 0x139f4 字节。未烧录，实际 DMA 分配及撕裂待用户复测。

用户要求清理 `build` 和 `build-perf` 后恢复标准 `build` 目录。当前构建与烧录统一使用 `idf.py build` 和 `idf.py flash monitor`；上文 `build-perf` 命令仅为历史记录。已移除临时目录忽略规则。

## PSRAM 缓存优化

PSRAM 对象池 + 60 行 DMA 的板上基线为约 30 FPS、TX 22.9ms、LVGL 26.0ms、Gap 6.4ms/帧，内部最低空闲堆 106,212B，UI 栈余量 824B。下一轮仅将指令缓存 16KiB→32KiB、数据缓存 32KiB→64KiB、数据缓存行 32B→64B，保持 PSRAM 对象池、60 行内部 DMA、80MHz SPI 与单 UI 任务。配置同步到 defaults 和生效 sdkconfig。更大的缓存消耗片上空间，不能把旧堆余量视为新预算；需复测初始化 DMA largest、最低堆、栈余量和条纹错位。收益待上板确认。

依据：[Espressif LVGL adapter 缓存优化建议](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/tools/esp_lvgl_adapter.html)。未启用 XIP 或增加绘制任务，便于单独比较缓存的影响。

缓存优化固件 IDF 6.1 构建通过（`build`，镜像 0x161f50 字节）。生成配置确认 instruction cache=0x8000、data cache=0x10000、data cache line=64。尚未烧录，收益与资源余量待板上日志。

## 固定 PSRAM 与缓存的 DMA 大小对比

60 行基线（32KiB 指令缓存 / 64KiB 数据缓存 / 64B 数据缓存行，PSRAM LVGL 池）：稳定 35.4–35.5 FPS，TX 约 19.7ms、LVGL 21.7ms、Gap 3.5ms/帧；内部最低堆 56,688B，UI 栈余量 840B。36.7 FPS 为单个采样窗口，不作为稳定基线。

当前对比版仅将两块 DMA 缓冲各从 60 行降至 40 行（总量 76,800B→51,200B，整屏分块 4→6，SPI 最大传输同步缩小），保持上述缓存、PSRAM 池、SPI 80MHz、绘制与调度不变。烧录后同一测试至少采样 10 秒，报告 FPS、TX、LVGL、Gap、pixels/frame、最低堆与栈余量，并观察撕裂。40 行板上结果待用户提供，不把历史内部池 37.2 FPS 当作本轮 40 行数据。

40 行对比版构建通过，用户板上日志连续 5 个窗口为 41.5 FPS，TX 约 17.6ms、LVGL 17.3ms、Gap 1.28ms/帧，pixels/frame=76,800。内部堆当前 118,967B、最低 83,128B，UI 栈余量 840B，PSRAM 当前 8,300,184B。用户反馈帧率提高后撕裂有所缓解，但仍存在。最终保留 PSRAM LVGL 池 + 优化缓存 + 40 行内部双 DMA；这些窗口不足 10 秒，不声称完成长期稳定性或防撕裂验收。
