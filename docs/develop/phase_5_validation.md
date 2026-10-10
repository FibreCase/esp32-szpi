# 第五阶段验证记录

日期：2026-10-08。

## 已实现

- Display 增加 FPS / Tearing 测试入口，使用整屏移动条纹观察撕裂；板上按完整刷新周期的最后一块 DMA 完成计帧，每秒汇总 FPS、TX、LVGL 和分块 Gap。共享 UI 通过模型获取指标；模拟器仅提供桌面预览。测量口径、使用步骤和资源归属见 [显示性能测试](display_performance_test.md)，实际板上数据待测。

- 新增 `szpi_ui` 组件，页面与状态模型接口只使用标准 C 与 LVGL。组件不创建任务，也不依赖 ESP-IDF、FreeRTOS 或外设；当前状态栏没有交互控件。
- 页面使用纯黑背景；主页顶部状态栏左侧显示 Wi-Fi 在线状态，中央标题为 `SZ-PI`，右侧显示本地时间。未完成 NTP 同步时显示 `--:--`。设置菜单及其详情页顶部左侧显示 `< 上一级标题`（Home、Settings 或 Audio），中间显示当前标题。
- 主页左划进入位于主页右侧的设置菜单，菜单按顺序列出 Network、Display、Camera、Audio、Storage、About；Camera 详情页显示 GC2145、320 x 240 和 RGB565 静态规格，并提供 Test 实时预览入口，进入启动、右划退出停止；测试页显示刷新 FPS、错误数和错误码，模拟器显示 Camera unavailable；卡片可点击进入位于父菜单右侧的详情页，并支持纵向滚动。Storage 页面以状态卡和容量详情呈现 SD 卡状态、容量、可用空间、文件系统与 SDMMC 速度，提供重试挂载和二次确认的 FAT32 格式化按钮；板上通过 szpi_app 状态模型与存储服务交互，模拟器只变更 mock 状态。Audio 控件绑定 audio service，其他演示控件不访问设备服务。详情页右划返回父菜单，设置页右划回主页；页面切换均带滑动动画，不显示返回按钮。
- About 显示设备、平台和固件版本；固件版本来自 ESP-IDF app description，模拟器使用独立版本号。板上 `Install update` 按钮通过 app 服务启动 OTA，状态显示下载进度和失败码；模拟器只模拟“不支持 OTA”的失败状态。OTA 流程与限制见 [OTA 设计](ota_design.md)。
- About 信息卡片之间保留 8 px 间距；OTA 状态和更新按钮位于信息卡片下方，通过纵向滚动查看，状态文字限定宽度并自动换行。
- 子菜单仅通过点击菜单卡片进入，设置菜单上的前进滑动被消耗，不转成点击；横向手势仅用于主页进入设置或返回父页面，纵向滚动用于浏览列表。
- 开关演示控件采用 62×36 像素外观，并扩大周围触摸区域；仅点击开关及其触摸区域切换本地演示状态。
- Audio 滑条卡片显示不可拖动的占比条；扬声器默认 50%，麦克风默认 100%。点击卡片进入独立调节页，使用大滑条更新音频服务并同步卡片预览；松开或按 Back 时保存两项设置到 NVS。调节页禁用划页手势，通过 `Back` 按钮带动画返回 Audio；其他菜单页仍通过手势返回。
- Audio 页面提供独立的 Audio Test 子页，可提交 440Hz 测试音、双麦声级采样和停止请求，并显示音频服务状态、峰值与 RMS。测试页面调用只经 szpi_app 桥接的有界音频服务 API；Linux simulator 使用 mock 状态，不访问宿主麦克风或扬声器。
- Wi-Fi 服务拿到 DHCP 地址后启动 SNTP 客户端（sdkconfig 默认 `pool.ntp.org`，可配置固定地址或当前 DHCP 网关，见 [SNTP 验证记录](sntp_validation.md)），采用中国标准时间（UTC+8）。UI 由 app 服务传入网络状态和格式化后的 `HH:MM`，共享页面不读取系统时间或访问网络。
- 板上由唯一 UI 任务每 20 ms 轮询 QMI8658A，并继续基于 X 轴加速度管理自动方向；IMU 状态不显示在当前页面。Linux 模拟器显示主机本地时钟并模拟 Wi-Fi 在线。
- 已实现基于 X 轴加速度的 180° 自动旋转：X≤−0.65g 连续 4 个可靠新样本切到倒立，X≥+0.65g 连续 4 个样本切回正立，滞回区保持方向；方向切换时更新 ST7789 镜像并反转触摸坐标。轴向依据用户确认的实测数据，详见 [硬件接线与轴向记录](../hardware_io.md)。实际屏幕翻转与触摸对齐待板上验收。
- `szpi_app` 保留原 runtime 创建的唯一 UI 任务，并管理板上 LVGL、显示、触摸的初始化与停止。旧硬件验证页面已从 app 移除，硬件服务及状态查询 API 仍在各自服务中。
- Linux SDL2 模拟器引用与固件相同的 `szpi_ui_sources.cmake`，窗口固定为 320×240、1:1 显示且不可调整大小；鼠标拖动模拟触摸划页；键盘右方向键模拟左划进入设置，左方向键模拟右划返回，B 键转成 mock BOOT 短按，Esc 退出。
- LVGL 通过 CMake 固定为官方 `v9.5.0` tag；构建目录为 `simulator/build`。本机为 Fedora 44、CMake 4.3.0、GCC 16.2.1、SDL2 2.32.74。
- UI 文案使用英文，状态栏字体采用 Noto Sans。屏幕根对象设置默认文本字体，子控件继承该样式；字形子集只打包 ASCII U+0020–U+007F，生成的 C 源码由 `szpi_ui_sources.cmake` 同时加入固件和模拟器，不需要运行时字体文件。资源使用 [Google Fonts Noto Sans](https://github.com/google/fonts/tree/main/ofl/notosans)，许可证为 SIL Open Font License 1.1，副本在 `components/szpi_ui/assets/fonts/OFL.txt`。生成器为 [lv_font_conv](https://github.com/lvgl/lv_font_conv) 1.5.3，16 px、4 bpp、未压缩。源字体 SHA-256：`bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d`。

从 [Google Fonts 下载 Noto Sans 可变字体](https://raw.githubusercontent.com/google/fonts/main/ofl/notosans/NotoSans%5Bwdth%2Cwght%5D.ttf)，保存为 `NotoSans-variable.ttf` 并确认 SHA-256 与上面一致，然后执行：

```sh
npx --yes --package=lv_font_conv@1.5.3 -- lv_font_conv \
  --font NotoSans-variable.ttf \
  -r '0x20-0x7F' \
  --size 16 --format lvgl --bpp 4 --no-compress \
  --lv-font-name szpi_ui_font_button --lv-include lvgl.h \
  -o components/szpi_ui/assets/fonts/szpi_ui_font_button.c
```

## 构建结果

- Linux simulator：`cmake -S simulator -B simulator/build` 配置成功；`cmake --build simulator/build` 成功。
- ESP-IDF 6.1：增量 `idf.py build` 与独立干净 defaults 构建均成功；后者在 `/tmp/szpi-phase5-defaults` 使用单独的 `sdkconfig`，生成 `esp32-szpi.bin`。
- 模拟器窗口运行、实际屏幕 / 触摸和长期运行尚未验证；没有执行烧录或设备操作。

## 尚待板上验收

- 状态栏和测试菜单的实际位置、颜色、字体大小，以及 Wi-Fi 状态和 NTP 校时结果；左右划页的板上响应。
- UI 停止与 DMA 完成时序、内存和任务栈余量。
- 连续运行 30 分钟及至少 50 次 UI 停止 / 启动。当前页面没有切页，因此切页验收需等产品页面加入后执行。

## Display 菜单实际绑定

Display 新增 Brightness 卡片，显示只读占比条，点击后进入位于右侧的独立大滑条页面；页面禁用导航手势，Back 动画返回 Display，左侧标题为 `< Display`。范围 10–100%；仅 slider 可拖动。Audio 的两个滑条绑定音频服务，并使用各自父页面返回。

共享事件契约新增 brightness changed/save，回调携带 uint32_t value（百分比）；测试 start/stop 的 value 为 0。事件仍由唯一 LVGL 线程同步发出，应用回调只记录待处理请求。UI 任务下次循环调用 display LEDC brightness 接口；成功后更新模型，失败打印日志并以实际已应用值同步 UI。松手或 Back 请求保存，仅值变更时写 NVS display/brightness 并 commit，不在拖动中逐步写 Flash。启动读 NVS，缺失或非法值回退 50%，读写错误可观察且不擦除 NVS。没有新增任务、队列或 DMA 缓冲。

FPS / Tearing 保留实际采样绑定；Screen 显示固定硬件 320×240，Theme 显示当前唯一 Dark 主题；Orientation 根据应用实际朝向显示 Auto / 0 deg 或 Auto / 180 deg，IMU 不可用时仅显示当前朝向。主题与分辨率是只读信息，不提供伪造的切换能力。Simulator 只模拟亮度值与页面反馈，不访问 LEDC/NVS。新功能上板亮度和重启恢复待验收。

本次 Display 绑定的 IDF 6.1 固件构建与 Linux simulator 构建均通过，diff 空白检查通过。未执行烧录或新增运行测试；硬件亮度、NVS 重启恢复及手势表现待上板确认。

2026-10-08：全部独立调节页大滑条由 264px 缩短为 240px，居中后左右各留 40px，避开触摸边缘死区。亮度 UI / 应用请求 / simulator 下限统一 10%；历史 NVS 值低于 10% 时按 10% 恢复，下次显式保存更新存储值。显示服务仍允许停止流程将背光关闭。固件与模拟器构建、已有共享 UI 回归测试和空白检查通过；端点触摸待上板复测。

2026-10-08：设置详情页按日常设置 / 信息在前、独立测试入口在后的顺序排列。Display 顺序为 Brightness、Screen、Theme、Orientation、FPS / Tearing；测试卡片显式放在最后，避免默认位置把它置于顶部。当前其他共享设置页没有额外独立测试入口。固件与 Linux simulator 构建通过。

## Camera 测试页补充（2026-10-10）

- Settings → Camera → Test 使用共享页面与页面生命周期事件；唯一 UI 任务在回调外启动 / 停止 runtime preview 服务，不新增任务。快速退出取消尚未处理的 START，重新进入等待上次 STOP 确认。采集失败不自动重试，退出后再次进入可显式重试。
- UI 按需分配并持有 153600 字节 PSRAM staging；RGB565 高字节优先帧转换为本机字节序并水平镜像，复制前等待上一轮 DMA。UI 完成复制及刷新后回传 generation / token 确认，camera 服务拥有并归还原帧。普通退出保留 staging 供下次使用；UI 停止时排空帧、解绑图像、等待 DMA 后释放 staging。
- 固件 IDF 6.1 与 Linux simulator 构建通过。主机测试 `tests/test_szpi_camera_ui.c` 通过：10 次页面进入 / 退出事件配对、坏图像描述拒绝、RGB565 图像显示及内容更新、故障 / 不可用提示、活动页销毁与重建。该测试不覆盖实际 camera / DMA 并发。
- 尚未烧录或上板验证：真实颜色、镜像方向、实测 FPS、快速划页的停止 / 重启、采帧故障与长期运行仍待验收。

## 设置页 NVS 核对（2026-10-10）

现有全部可调项为 Wi-Fi 网络、Display 亮度、Audio 扬声器音量和麦克风增益；四者已有 NVS 启动恢复与保存路径，键名和保存时机见 README。补齐 UI 协作停止时保存：在销毁共享页面前，应用最后一笔待处理亮度并提交变化值，向 audio owner 提交保存请求，避免尚未松开滑条或收到 STOP 后未进入下一轮 UI 循环时漏存。音频通过原有有界队列提交，队列满或 NVS 写入失败打印错误；不声称已保存，不擦除 NVS。

设备名 hostname 也由 `szpi_wifi / hostname` 保存，但当前设置页未提供编辑控件。SNTP 和 OTA 地址按用户确认继续使用 sdkconfig，不纳入运行时 NVS 设置。模拟器不访问 NVS。硬件断电重启恢复及故障注入尚未上板验收。

本次设置保存收尾改动通过 IDF 6.1 构建（独立网关模式构建目录）与 diff 空白检查。未执行烧录，未将编译通过当作 NVS 重启恢复的板上验收。

2026-10-10：About 新增只读 Hostname 卡片，由 szpi_app 从 Wi-Fi 状态复制当前 hostname 到共享 UI 模型；默认设备名为 szpi，未获取状态时显示 Unavailable。最多 32 字符长名称换行，OTA 控件顺延；模拟器显示 szpi。
