# 第五阶段验证记录

日期：2026-10-08。

## 已实现

- Display 增加 FPS / Tearing 测试入口，使用整屏移动条纹观察撕裂；板上按完整刷新周期的最后一块 DMA 完成计帧，每秒汇总 FPS、TX、LVGL 和分块 Gap。共享 UI 通过模型获取指标；模拟器仅提供桌面预览。测量口径、使用步骤和资源归属见 [显示性能测试](display_performance_test.md)，实际板上数据待测。

- 新增 `szpi_ui` 组件，页面与状态模型接口只使用标准 C 与 LVGL。组件不创建任务，也不依赖 ESP-IDF、FreeRTOS 或外设；当前状态栏没有交互控件。
- 页面使用纯黑背景；主页顶部状态栏左侧显示 Wi-Fi 在线状态，中央标题为 `SZ-PI`，右侧显示本地时间。未完成 NTP 同步时显示 `--:--`。设置菜单及其详情页顶部左侧显示 `< 上一级标题`（Home、Settings 或 Audio），中间显示当前标题。
- 主页左划进入位于主页右侧的设置菜单，菜单列出 Display、Network、Audio、Storage、About；卡片可点击进入位于父菜单右侧的详情页，并支持纵向滚动。详情页展示只影响本地界面的演示控件：Display / About 信息行、Network / Storage 开关、Audio 滑条；滑条显示当前百分比，控件不绑定设备服务。详情页右划返回父菜单，设置页右划回主页；页面切换均带滑动动画，不显示返回按钮。
- 子菜单仅通过点击菜单卡片进入，设置菜单上的前进滑动被消耗，不转成点击；横向手势仅用于主页进入设置或返回父页面，纵向滚动用于浏览列表。
- 开关演示控件采用 62×36 像素外观，并扩大周围触摸区域；仅点击开关及其触摸区域切换本地演示状态。
- Audio 滑条卡片显示不可拖动的占比条；点击卡片进入独立调节页，使用大滑条修改本地演示值并同步卡片预览。调节页禁用划页手势，通过 `Back` 按钮带动画返回 Audio；其他菜单页仍通过手势返回。
- Wi-Fi 服务拿到 DHCP 地址后启动 `pool.ntp.org` 的 SNTP 客户端，采用中国标准时间（UTC+8）。UI 由 app 服务传入网络状态和格式化后的 `HH:MM`，共享页面不读取系统时间或访问网络。
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

Display 新增 Brightness 卡片，显示只读占比条，点击后进入位于右侧的独立大滑条页面；页面禁用导航手势，Back 动画返回 Display，左侧标题为 `< Display`。范围 10–100%；仅 slider 可拖动。Audio 的两个滑条仍为测试值，使用各自父页面返回。

共享事件契约新增 brightness changed/save，回调携带 uint32_t value（百分比）；测试 start/stop 的 value 为 0。事件仍由唯一 LVGL 线程同步发出，应用回调只记录待处理请求。UI 任务下次循环调用 display LEDC brightness 接口；成功后更新模型，失败打印日志并以实际已应用值同步 UI。松手或 Back 请求保存，仅值变更时写 NVS display/brightness 并 commit，不在拖动中逐步写 Flash。启动读 NVS，缺失或非法值回退 50%，读写错误可观察且不擦除 NVS。没有新增任务、队列或 DMA 缓冲。

FPS / Tearing 保留实际采样绑定；Screen 显示固定硬件 320×240，Theme 显示当前唯一 Dark 主题；Orientation 根据应用实际朝向显示 Auto / 0 deg 或 Auto / 180 deg，IMU 不可用时仅显示当前朝向。主题与分辨率是只读信息，不提供伪造的切换能力。Simulator 只模拟亮度值与页面反馈，不访问 LEDC/NVS。新功能上板亮度和重启恢复待验收。

本次 Display 绑定的 IDF 6.1 固件构建与 Linux simulator 构建均通过，diff 空白检查通过。未执行烧录或新增运行测试；硬件亮度、NVS 重启恢复及手势表现待上板确认。

2026-10-08：全部独立调节页大滑条由 264px 缩短为 240px，居中后左右各留 40px，避开触摸边缘死区。亮度 UI / 应用请求 / simulator 下限统一 10%；历史 NVS 值低于 10% 时按 10% 恢复，下次显式保存更新存储值。显示服务仍允许停止流程将背光关闭。固件与模拟器构建、已有共享 UI 回归测试和空白检查通过；端点触摸待上板复测。

2026-10-08：设置详情页按日常设置 / 信息在前、独立测试入口在后的顺序排列。Display 顺序为 Brightness、Screen、Theme、Orientation、FPS / Tearing；测试卡片显式放在最后，避免默认位置把它置于顶部。当前其他共享设置页没有额外独立测试入口。固件与 Linux simulator 构建通过。
