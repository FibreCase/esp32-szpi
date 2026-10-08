# 第五阶段验证记录

日期：2026-10-08。

## 已实现

- 新增 `szpi_ui` 组件，页面、模型更新和按钮事件接口只使用标准 C 与 LVGL。组件不创建任务，也不依赖 ESP-IDF、FreeRTOS 或外设。
- 页面为 320×240 空白底色、顶部 IMU 三轴加速度读数和居中按钮。点击事件通过回调送到 app；状态模型更新按钮文案和读数，不在回调里访问硬件。
- 板上由唯一 UI 任务每 20 ms 轮询 QMI8658A，显示 X/Y/Z 加速度，单位 g；IMU 初始化或采样失败时显示状态文字并保留页面可用。模拟器使用水平放置的 mock 值 `(0.00, 0.00, 1.00) g`。
- 已实现基于 X 轴加速度的 180° 自动旋转：X≤−0.65g 连续 4 个可靠新样本切到倒立，X≥+0.65g 连续 4 个样本切回正立，滞回区保持方向；方向切换时更新 ST7789 镜像并反转触摸坐标。轴向依据用户确认的实测数据，详见 [硬件接线与轴向记录](../hardware_io.md)。实际屏幕翻转与触摸对齐待板上验收。
- `szpi_app` 保留原 runtime 创建的唯一 UI 任务，并管理板上 LVGL、显示、触摸的初始化与停止。旧硬件验证页面已从 app 移除，硬件服务及状态查询 API 仍在各自服务中。
- Linux SDL2 模拟器引用与固件相同的 `szpi_ui_sources.cmake`，窗口固定为 320×240、1:1 显示且不可调整大小；B 键转成 mock BOOT 短按，Esc 退出。
- LVGL 通过 CMake 固定为官方 `v9.5.0` tag；构建目录为 `simulator/build`。本机为 Fedora 44、CMake 4.3.0、GCC 16.2.1、SDL2 2.32.74。
- UI 文案使用英文，按钮字体统一为 Noto Sans。屏幕根对象设置默认文本字体，子控件继承该样式；字形子集只打包 ASCII U+0020–U+007F，生成的 C 源码由 `szpi_ui_sources.cmake` 同时加入固件和模拟器，不需要运行时字体文件。资源使用 [Google Fonts Noto Sans](https://github.com/google/fonts/tree/main/ofl/notosans)，许可证为 SIL Open Font License 1.1，副本在 `components/szpi_ui/assets/fonts/OFL.txt`。生成器为 [lv_font_conv](https://github.com/lvgl/lv_font_conv) 1.5.3，16 px、4 bpp、未压缩。源字体 SHA-256：`bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d`。

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

- 显示按钮的实际位置、颜色、字体大小和触摸命中区域。
- UI 停止与 DMA 完成时序、内存和任务栈余量。
- 连续运行 30 分钟及至少 50 次 UI 停止 / 启动。当前页面没有切页，因此切页验收需等产品页面加入后执行。
