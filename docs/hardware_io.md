# 嘉立创实战派 ESP32-S3 GPIO 与外设连接

## 来源与读表约定

依据本项目的 [hardware_schematic.pdf](hardware_schematic.pdf)，原理图 `ESP32-S3-V1_0_1`，版本 **V1.0.1**，共 3 页：P1 主控 / I²C / 扩展器 / TF，P2 USB / LCD / 触摸 / 摄像头，P3 音频。已核对三页接线与跨页网络标签。器件型号、屏幕规格及标称能力补充自 [hareware_description.png](hareware_description.png)（文件名沿用原文件）。接线以原理图为依据，参数表用于补充型号与规格；标称能力不代表项目当前已启用该配置。摄像头型号 **GC2145** 由用户确认。

主控 U1 为 **ESP32-S3-WROOM-1-N16R8**（16MB Flash、8MB PSRAM）。本文的 GPIO 数字是 ESP-IDF 中使用的 GPIO 编号；“模组脚号”是 U1 的物理焊盘号，两者不能混用。方向均相对 ESP32-S3；方向用于描述板载用途，不代表 GPIO 的全部复用能力。PCA9557 的 IO0/1/2 为扩展器端口，不能传给 `gpio_set_level()`。

## 硬件型号与标称参数

下表整理硬件说明图，并结合原理图给出编码关联。

| 类别 | 型号 / 规格 | 编码关联或范围 |
| --- | --- | --- |
| 主控模组 | ESP32-S3-WROOM-1-N16R8；Xtensa 32-bit LX7 双核，最高 240MHz；内部 SRAM 512KB；PSRAM 8MB；Flash 16MB | 240MHz 为标称上限，实际 CPU 频率以 sdkconfig 为准 |
| 无线 | 2.4GHz Wi-Fi 802.11 b/g/n，说明图标注 40MHz 带宽；Bluetooth 5 LE / Bluetooth Mesh | 按应用需求启用无线组件；不能据此认为支持经典蓝牙 |
| 指令能力 | AI 向量指令，用于神经网络计算和信号处理加速 | 是否使用取决于所选库及实现 |
| 显示屏 | **ST7789，2.0 英寸 IPS，320×240，SPI** | MOSI=40、SCLK=41、DC=39、BL=42，CS=PCA9557 IO0 |
| 电容触摸 | **FT6336，I²C** | 原理图使用系列标注 FT6X36；地址 0x38，无中断 GPIO |
| 摄像头 | **GC2145**（用户确认），使用板上 DVP 接口 | DVP 数据 / 时钟接线见摄像头小节；SCCB 共用 GPIO1/2 |
| 姿态传感器 | QMI8658，三轴加速度 + 三轴陀螺仪，I²C | 原理图具体型号 QMI8658A，地址 0x6A |
| 音频 DAC / Codec | ES8311，单通道；说明图标注 I²C 接口 | I²C 为寄存器控制接口，播放数据通过 I²S GPIO45 输入芯片 |
| 音频 ADC | ES7210，四通道，开发板使用三个通道 | I²C 控制，I²S GPIO12 接收；两路麦克风 + 一路 Codec 输出回采 |
| 音频功放 | NS4150B，单声道 D 类功放 | PA_EN=PCA9557 IO1；模拟输入来自 ES8311 |
| 麦克风 | ZTS6216，两路，模拟输出 | MIC1/MIC2 经 ES7210 转成数字音频，不直接接主控 ADC |
| 喇叭 | DB1811AB50，1811 音腔喇叭，标称 1W | 接 J7 功放输出；1W 是喇叭标称，不是软件可直接设定的输出功率 |
| USB HUB | CH334F，USB 2.0 HUB | 同一 Type-C 连接原生 USB 与 CH340K 串口两条路径 |
| USB 转串口 | CH340K，说明图标注波特率最高 2Mbps | UART0 TX=43、RX=44；实际下载 / 日志波特率按链路情况配置 |
| 电源 | SY8088AAC，双路，每路标称 1A | 原理图为 U6 音频 AU_3V3、U8 主系统 3V3；不是每个外部接口均可提供 1A |
| 外部接口 | 两路 GH1.25，提供 5V / 3.3V；说明图列出 GPIO、CAN、UART、PWM 等用途 | J1 固定接共享 I²C，J2 接 GPIO10/11；复用功能需软件配置并满足外部电路要求 |
| TF 卡 | 1-SD 模式 | 使用 SDMMC 1-bit：CLK=47、CMD=48、D0=21 |
| Type-C | 供电、程序下载、调试、USB 数据通信 | 用途对应具体 USB / UART 路径，不自动意味着所有 USB 功能已启用 |
| 按键 | 一个复位键、一个用户自定义键 | 用户键与 BOOT 共用 SW2 / GPIO0，低有效 |

## GPIO 总表

| GPIO | U1 模组脚号 | 原理图网络 | 板载连接 / 用途 | 方向与备注 |
| --- | --- | --- | --- | --- |
| 0 | 27 | IO0_BOOT | SW2 BOOT / 用户按键；Q1 自动下载电路 | 输入，10kΩ 上拉，按下为低；启动配置脚 |
| 1 | 39 | IO1_I2C_SDA | 所有板载 I²C 器件、触摸、摄像头控制、J1.2 | 双向，板载 1kΩ 上拉到 3.3V |
| 2 | 38 | IO2_I2C_SCL | 同一 I²C 总线；J1.1 | 时钟，板载 1kΩ 上拉到 3.3V |
| 3 | 15 | IO3_DVP_VSYNC | 摄像头 J6.7 VSYNC | 输入；启动配置脚 |
| 4 | 4 | IO4_DVP_D6 | 摄像头 J6.14 D6 | 输入 |
| 5 | 5 | IO5_DVP_XCLK | 摄像头 J6.13 XCLK | 输出；本版原理图用途是摄像头时钟 |
| 6 | 6 | IO6_DVP_D5 | 摄像头 J6.16 D5 | 输入 |
| 7 | 7 | IO7_DVP_PCLK | 摄像头 J6.17 PCLK | 输入 |
| 8 | 12 | IO8_DVP_D2 | 摄像头 J6.22 D2 | 输入 |
| 9 | 17 | IO9_DVP_D7 | 摄像头 J6.12 D7 | 输入 |
| 10 | 18 | IO10 | 多用外部接口 J2.2 | 外部通用 IO |
| 11 | 19 | IO11 | 多用外部接口 J2.1 | 外部通用 IO |
| 12 | 20 | IO12_I2S_DI | ES7210 U10.11 SDOUT1，经 R36 51Ω | 音频数据输入；U10.12 经 R37 可选接入，R37 标记 NC |
| 13 | 21 | IO13_I2S_WS | ES7210 U10.10 LRCK、ES8311 U9.8 LRCK | 共享音频帧时钟 |
| 14 | 22 | IO14_I2S_BCK | ES7210 U10.9 SCLK、ES8311 U9.6 SCLK | 共享音频位时钟 |
| 15 | 8 | IO15_DVP_D4 | 摄像头 J6.18 D4 | 输入 |
| 16 | 9 | IO16_DVP_D0 | 摄像头 J6.19 D0 | 输入 |
| 17 | 10 | IO17_DVP_D3 | 摄像头 J6.20 D3 | 输入 |
| 18 | 11 | IO18_DVP_D1 | 摄像头 J6.21 D1 | 输入 |
| 19 | 13 | IO19_USB_D- | CH334F U5.7 DM3 | 原生 USB D-，经 HUB 接到 Type-C |
| 20 | 14 | IO20_USB_D+ | CH334F U5.8 DP3 | 原生 USB D+，经 HUB 接到 Type-C |
| 21 | 23 | IO21_SD_DAT0 | TF 卡座 J3.7 DAT0 | SDMMC 单线模式，双向 |
| 35 | 28 | 无外部网络，NC | 原理图外部未连接；N16R8 Octal PSRAM 占用 | 保留给模组内存，不能当空闲 IO |
| 36 | 29 | 无外部网络，NC | 同上 | 保留给模组内存 |
| 37 | 30 | 无外部网络，NC | 同上 | 保留给模组内存 |
| 38 | 31 | IO38_I2S_MCK | ES7210 U10.5 MCLK、ES8311 U9.2 MCLK | 共享音频主时钟，通常由主控输出 |
| 39 | 32 | IO39_LCD_DC | TFT J4.1 DC | 输出，区分命令 / 数据 |
| 40 | 33 | IO40_LCD_MOSI | TFT J4.2 SDA | SPI MOSI 输出，不是 I²C SDA |
| 41 | 34 | IO41_LCD_SCK | TFT J4.5 SCL | SPI SCLK 输出，不是 I²C SCL |
| 42 | 35 | IO42_LCD_BL | 经 R23 驱动 Q2 背光 MOS 管 | 输出 / PWM；按电路推导低电平点亮，见 LCD 小节 |
| 43 | 37（TXD0） | U0TXD | CH340K U4.9 RXD | UART0 TX 输出 |
| 44 | 36（RXD0） | U0RXD | CH340K U4.8 TXD | UART0 RX 输入 |
| 45 | 26 | IO45_I2S_DO | ES8311 U9.9 DSDIN | 音频数据输出；启动配置脚 |
| 46 | 16 | IO46_DVP_HREF | 摄像头 J6.9 HREF；R3 10kΩ 下拉 | 输入用途；启动配置脚 |
| 47 | 24 | IO47_SD_CLK | TF 卡座 J3.5 CLK | SDMMC 时钟输出 |
| 48 | 25 | IO48_SD_CMD | TF 卡座 J3.3 CMD | SDMMC 命令，双向 |

GPIO22～34 不在 U1 外部引脚列表内，不作为本板可分配外设引脚。关于 Octal PSRAM 对 GPIO33～37 的占用及启动配置脚，参见 [乐鑫 GPIO 文档](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/api-reference/peripherals/gpio.html)。UART0 的 GPIO43/44 对应关系参见 [乐鑫硬件设计指南](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html)。

### 电源与复位引脚

| U1 模组脚号 | 信号 | 连接 |
| --- | --- | --- |
| 1、40、41 | GND | 板上地；41 为底部焊盘 |
| 2 | 3V3 | 3.3V 电源 |
| 3 | EN / RESET | R1 10kΩ 上拉、C7 1µF；SW1 按下接地；Q1 自动下载复位 |

`RESET` 同时连接 PCA9557 U2.15、LCD J4.4、触摸 J5.5、摄像头 J6.6。它不是独立 GPIO，不能通过某个主控 IO 单独复位上述器件。

## 按外设配置

### 共享 I²C 总线

固定接线为 **SDA=GPIO1、SCL=GPIO2**。R4/R6 为 1kΩ 外部上拉。I²C 控制器实例编号可由软件选择，不由原理图决定；总线速率应按所接全部器件的规格设置。

| 设备 | 位号 / 位置 | 7-bit 地址（原理图标注） | 中断 / 备注 |
| --- | --- | --- | --- |
| PCA9557 IO 扩展器 | U2，P1 | 0x19 | A0 上拉，A1/A2 接地；控制 LCD_CS、PA_EN、DVP_PWDN |
| QMI8658A 六轴传感器 | U3，P1 | 0x6A | INT1/INT2 均 NC，采用轮询 |
| FT6336 电容触摸屏（原理图标 FT6X36） | J5，P2 | 0x38 | J5.2 EINT 为 NC，采用轮询 |
| ES8311 音频 Codec | U9，P3 | 0x18 | 播放输出；ASDOUT 未接主控 |
| ES7210 音频 ADC | U10，P3 | 0x41 | 录音输入；INT 为 NC |
| 摄像头 SCCB | J6.3 SDA、J6.5 SCL，P2 | 0x3C（乐鑫驱动 / GC2145 手册） | 与板载 I²C 共用 GPIO1/2，协调总线访问 |
| 外部 I²C 接口 | J1，P1 | 由外接设备决定 | 外接器件应避免地址冲突 |

上述固定地址直接作为 ESP-IDF 的 7-bit 设备地址使用，不左移一位；8-bit 读写地址只在明确要求这种格式的接口中使用。

### PCA9557：三个间接控制信号

| 扩展器端口 | U2 脚号 | 网络 | 目标 |
| --- | --- | --- | --- |
| IO0 / bit0 | 6 | LCD_CS | TFT J4.3 片选 |
| IO1 / bit1 | 7 | PA_EN | NS4150B U11.1 CTRL，功放控制 |
| IO2 / bit2 | 9 | DVP_PWDN | 摄像头 J6.8 掉电控制 |
| IO3～IO7 | 10～14 | NC | 未连接板载外设，也未引出到本文接口 |

编码时先初始化 I²C 和 PCA9557，再设置相应端口方向 / 输出。更新某一位时保留另外两位，避免控制屏幕时同时改变功放或摄像头状态。`PA_EN` 高开启、低关闭；GC2145 的 `DVP_PWDN` 高掉电、低运行，依据见下方“已选定的初始驱动配置”。

### LCD 与触摸

硬件说明图明确显示屏为 **ST7789，2.0 英寸 IPS，320×240，SPI 接口**，触摸芯片为 **FT6336**。原理图的 FT6X36 是系列标注，与此型号信息一起作为驱动选型依据。

| LCD 信号 | 连接 | 驱动配置提示 |
| --- | --- | --- |
| MOSI | GPIO40，J4.2 | SPI 数据输出 |
| SCLK | GPIO41，J4.5 | SPI 时钟 |
| DC | GPIO39，J4.1 | 主控 GPIO 控制 |
| CS | PCA9557 IO0，J4.3 | 扩展器控制，不是 ESP32 GPIO；低有效 |
| RST | RESET，J4.4 | 共用硬件复位；没有独立 reset GPIO |
| BL | GPIO42，经 Q2 | 背光亮度可用 PWM 控制 |
| MISO | 未连接 | 本接线不支持 SPI 读回 |

Q2 为 P 沟道 MOS 管，控制背光高侧供电，GPIO42 侧有 R21 10kΩ 下拉。**GPIO42 低为背光开，高为关，已由本板官方例程确认；使用 PWM 时启用输出反相。**

使用只接受主控 GPIO 的 SPI / LCD 配置时，CS 与 reset 应按该 API 的“未使用 GPIO”方式配置（常见为 `-1`），由板级代码另行管理片选。PCA9557 的 IO0 不能写成 `cs_gpio_num=0`，否则会误操作 BOOT GPIO0。驱动选型可按 ST7789、标称 320×240 确定；具体 SPI host、SPI 时钟、颜色顺序、反色、旋转及显示偏移仍需结合屏幕模组资料和实物测试设置。若驱动按竖屏坐标初始化为 240×320，需配置轴交换 / 旋转来匹配所需方向，不能只机械填入宽高。

触摸接口 J5：1=3V3、2=EINT（NC）、3=SCL(GPIO2)、4=SDA(GPIO1)、5=RESET、6=GND；7/8 为接地固定脚。无触摸中断 GPIO，应轮询读取触点。

### SD / TF 卡（单线 SDMMC）

```c
// 配置 SDMMC slot 时的板级引脚值；控制器实例由软件选择。
// width = 1
// clk   = GPIO_NUM_47
// cmd   = GPIO_NUM_48
// d0    = GPIO_NUM_21
```

DAT1（J3.8）未连接，DAT2（J3.1）、DAT3（J3.2）只有上拉，没有接入主控，因此不能直接配置 4-bit 模式。卡座 CD 检测触点为 NC，没有独立插卡检测 GPIO。R7～R10 为 51kΩ 上拉，分别位于 DAT0、CLK、CMD、DAT3；是否开启额外内部上拉按 SDMMC 驱动要求处理。

当前 `szpi_storage` 初始按 20MHz、1-bit、internal pull-up 配置，挂载路径 `/sdcard`，且只接受 FAT32；挂载失败时保留卡与 diskio 以支持后续显式格式化。SD 总线时钟、卡兼容性、容量和 FAT32 读写尚未上板验证，记录见[第四阶段验证](develop/phase_4_validation.md)。

### DVP 摄像头

实际摄像头型号为 **GC2145**（用户确认）。当前适配使用固定版本 espressif/esp32-camera 2.1.8；GPIO 接线仍以本板原理图为准，构建与实物探测结果见[第三阶段验证记录](develop/phase_3_validation.md)。

| camera_config_t 字段 / 功能 | GPIO / 控制 | J6 脚号 |
| --- | --- | --- |
| pin_d0 | 16 | 19 |
| pin_d1 | 18 | 21 |
| pin_d2 | 8 | 22 |
| pin_d3 | 17 | 20 |
| pin_d4 | 15 | 18 |
| pin_d5 | 6 | 16 |
| pin_d6 | 4 | 14 |
| pin_d7 | 9 | 12 |
| pin_xclk | 5 | 13 |
| pin_pclk | 7 | 17 |
| pin_vsync | 3 | 7 |
| pin_href | 46 | 9 |
| SCCB SDA | 1 | 3 |
| SCCB SCL | 2 | 5 |
| PWDN | PCA9557 IO2 | 8 |
| RESET | 共用 RESET 网络 | 6 |

D0～D7 顺序是 **`{16, 18, 8, 17, 15, 6, 4, 9}`**，不能按 GPIO 大小排序。SCCB 字段名按实际使用的 esp32-camera 版本确认。

对于只支持主控 GPIO 的 `pin_pwdn` / `pin_reset`，使用其“未使用”值（通常 `-1`）；初始化摄像头前，板级代码先通过 PCA9557 IO2 退出掉电状态。摄像头 reset 不能通过单独 GPIO 操作。

J6 的电源 / 未用脚：2、15 接 GND；11 接 2.8V；1、4、10、23、24 标记 NC。2.8V 来自 U7 ME6211；PWDN 网络 R25 10kΩ 上拉到 3.3V。摄像头型号已确认为 GC2145；查询确认的 SCCB 地址、PWDN 极性及初始 XCLK / 格式配置见下方默认配置。

### 音频 I²S

| 信号 | GPIO | 去向 |
| --- | --- | --- |
| MCLK | 38 | ES8311、ES7210 共用 |
| BCLK | 14 | ES8311、ES7210 共用 |
| WS / LRCK | 13 | ES8311、ES7210 共用 |
| DOUT（主控发送） | 45 | ES8311 DSDIN |
| DIN（主控接收） | 12 | ES7210 SDOUT1/TDMOUT，经 R36 51Ω |

ES8311 的 ASDOUT（U9.7）标记 NC，不能将其当作板载录音数据源；录音走 ES7210。ES7210 SDOUT2/TDMIN（U10.12）通过 R37 0Ω 可选接到同一 GPIO12，但 R37 标记 **NC / 不装**，默认使用 SDOUT1。

两个音频芯片共用 MCLK、BCLK、WS，配置录音和播放时必须协调时钟及格式，不能把它们当作两套独立引脚总线。原理图不能确定主从模式、采样率、位宽、通道 / TDM 排列；这些参数需由音频驱动统一设置。

说明图标注 ES7210 为四通道 ADC，开发板使用三个通道。结合原理图，CH1 为 ZTS6216 麦克风 MIC1（MIC1P/N），CH2 为 ZTS6216 麦克风 MIC2（MIC2P/N），CH3 为 ES8311 OUTP/N 经 R34/R35 接到 MIC3P/N 的模拟回采；MIC4P/N 标记 NC。因此第三路不是第三颗麦克风。NS4150B 功放输入来自 ES8311 OUTP/N，输出到 J7 喇叭接口；功放控制使用 PCA9557 IO1。说明图标注配套喇叭为 **DB1811AB50、1811 音腔、1W**，功放为单声道 D 类；录音通道选择与回采用途应在 ES7210 驱动中明确配置。

### USB、UART 与按键

Type-C 的 USB D+/D- 先连接 CH334F HUB 上行端。HUB 下行端口 3 接 ESP32 原生 USB GPIO19/20；下行端口 4 接 CH340K USB 转串口，再接 UART0 GPIO43/44。因此同一 Type-C 接口包含两条不同的通信路径。

CH340K 的 DTR / RTS 经 Q1 控制 `RESET` 和 GPIO0，实现自动下载；DTR / RTS 不占用额外主控 GPIO。UART0 用于串口下载 / 日志时，保留 GPIO43/44。硬件说明图标注 CH340K 最高波特率 2Mbps；这是器件标称能力，项目的下载和日志波特率需分别配置并验证。

SW1 为硬件复位键，不可作为普通 GPIO 按键读取。SW2 是 GPIO0 上的 BOOT / 用户共用按键，按下为低；启动时的按键状态会影响下载模式，运行阶段使用时应消抖。

### 外部接口

| 引脚 | J1（I²C） | J2（多用） |
| --- | --- | --- |
| 1 | GPIO2 / SCL | GPIO11 |
| 2 | GPIO1 / SDA | GPIO10 |
| 3 | 3V3，经滤波器 | 3V3，经滤波器 |
| 4 | VBUS，经滤波器 | VBUS，经滤波器 |
| 5 | GND | GND |

硬件说明图将两路外部接口标为 **GH1.25**，原理图连接器型号为 **HC-GH-5PWT**。接线时按连接器引脚号核对，不能由观察方向推断左右顺序；VBUS 与 3V3 是不同电源网络，信号 GPIO 使用 3.3V 逻辑。

说明图列出的 CAN、UART、PWM 等为接口扩展用途：J1 的 GPIO1/2 已与多个板载 I²C 器件共享，改作其他协议会影响这些器件；J2 的 GPIO10/11 更适合单独分配外部功能。原理图未提供 CAN 收发器，不能将这两根 GPIO 当作 CANH/CANL 总线直接连接。

## 已选定的初始驱动配置

用户已授权采用通用方案并查询资料。以下作为编码默认值，查询日期为 **2026-10-07**；相关驱动配置已落代码，但对应外设尚未上板验证。板级例程中已有的参数优先采用；总线实例、保守频率和缓冲策略是本项目的工程选择，不是所有同型号器件通用的硬件标准。

### I²C 与扩展器启动状态

- 共享总线先选 `I2C_NUM_0`、100kHz，SDA=1、SCL=2；稳定后可按全部器件支持情况提高到 400kHz。GC2145 手册规定 SCCB 最高 400kHz。
- 所有设备共用一个总线生命周期。摄像头接入时复用已配置端口，避免第二次安装控制 GPIO1/2 的总线；所选 esp32-camera 版本需与 ESP-IDF 6.1 的 I²C 驱动兼容。
- PCA9557 IO0：LCD_CS **0=选中，1=释放**；IO1：PA_EN **0=关闭，1=开启**；IO2：GC2145 PWDN **1=掉电，0=运行**。依据 [立创显示例程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/lcd-display.html)、[立创功放说明](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/introduction.html)及 [GalaxyCore GC2145 手册 §4.2、§8.2](https://www.camemaker.com/shop/gc2145-galaxycore-gc2145-2mp-fsi-rolling-30fps-2147/document/183)。

| PCA9557 寄存器 | 地址 | 本项目初始化写入值 | 用途 |
| --- | --- | --- | --- |
| Output Port | 0x01 | 0x05 | CS 释放、功放关闭、摄像头掉电 |
| Polarity Inversion | 0x02 | 0x00 | 输入读取不反相；此寄存器不改变输出极性 |
| Configuration | 0x03 | 0xF8 | IO0～2 输出，IO3～7 保持输入 |

按表中顺序先设置输出锁存值，再使能输出方向，避免刚切到输出时出现非预期状态。初始化后保存输出寄存器影子值，带锁修改指定 bit，并在 I²C 写成功后更新影子值。PCA9557 IO0 为开漏输出，置 1 时释放，由本板 LCD_CS 上的 R16 上拉。寄存器与开漏行为参照 [PCA9557 器件手册 §7](https://www.ti.com/lit/ds/symlink/pca9557.pdf)。

### ST7789 显示与 FT6336 触摸

| 参数 | 初始值 | 依据 |
| --- | --- | --- |
| LCD SPI host / 频率 | SPI2_HOST / 80MHz | 当前 CONFIG_SZPI_DISPLAY_SPI_CLOCK_HZ 覆盖为 80MHz；board 保守回退值 20MHz，mode 2，稳定性待复测 |
| SPI mode | 2 | 本板官方例程 |
| 命令 / 参数位宽 | 8 / 8 | 常规 ST7789 SPI 配置 |
| 像素格式 / 元素顺序 | RGB565，16bit / RGB | 本板官方例程 |
| 逻辑分辨率 | 320×240 横屏 | 与硬件说明及本板例程一致 |
| invert_color | true | 本板官方例程 |
| swap_xy | true | 本板官方例程 |
| mirror_x / mirror_y | true / false | 本板官方例程 |
| x_gap / y_gap | 0 / 0 | 初始选择；例程未额外设置偏移 |
| CS / reset GPIO 配置 | -1 / -1 | CS 由扩展器控制；reset 共用硬件线 |
| 背光 PWM | LEDC 低速，5kHz，10bit，output_invert=true | 本板官方例程；GPIO42 低有效 |
| 背光资源 | LEDC_TIMER_1、LEDC_CHANNEL_1 | 项目分配，避免与摄像头占用同一通道 |
| 触摸轮询周期 | 20ms | 项目初始选择，无中断 GPIO |

按本板官方例程，初始化顺序为 CS 保持释放 → `esp_lcd_panel_reset()` → 扩展器拉低 CS → `esp_lcd_panel_init()`；本 SPI 总线只连接此屏时随后可保持 CS 为低。reset GPIO 为 -1，CS 释放期间的驱动软件复位命令不会被面板接收，因此不能把该 API 成功视为面板已完成软件复位；面板依靠板上共用硬件 RESET，不能单独操作共用 RESET。完成面板初始化和清屏后，再打开显示及背光。初始缓冲选 RGB565；向屏幕发送时按驱动要求处理字节序，RGB/BGR 元素顺序与 RGB565 高低字节交换是两个不同问题。方向、反色和背光配置依据 [立创 LCD 官方例程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/lcd-display.html)。

FT6336 使用地址 0x38、轮询方式。触摸坐标需转换到同一 320×240 横屏坐标系；先读取原始坐标，再用四角触摸确认 swap / mirror 映射，不能只因 LCD 开启 swap_xy 就假定触摸驱动会自动同步。

### QMI8658A 与 BOOT 输入

QMI8658A 使用共享 board I²C 总线上的 0x6A device handle，100kHz、5ms 单事务超时；驱动复位并核对 WHO_AM_I=0x05 与复位结果，配置 CTRL1 小端自增、±4g / 250Hz 加速度及 ±512dps / 224.2Hz 六轴同步采样。同步读取按 QST QMI8658A Rev A 的 STATUSINT 锁定 / 解锁流程读取 AX_L 起始的 12 字节。加速度使用 8192 LSB/g，陀螺仪 64 LSB/dps；倾角只按静止重力计算 roll / pitch，不提供 yaw。实现与逻辑验证状态见 [第二阶段附加验证记录](develop/phase_2_extra_validation.md)。

SW2 / BOOT 仅由 GPIO0 输入读取，不启用内部上下拉或中断；UI 任务每 4ms 采样，20ms 消抖，稳定按下后 800ms 报告一次长按。GPIO0 启动 / 下载电气行为仍由板载上拉和自动下载电路决定，运行期适配不会驱动该脚。

第二阶段已按上表实现初始配置：SPI2 80MHz（配置覆盖）/ mode 2、GPIO40/41/39、PCA9557 IO0 低有效片选、ST7789 官方 reset / CS 初始化顺序与 RGB565、LEDC timer1/channel1 背光，以及 FT6336 新 I²C API 轮询。用户确认修正 reset / CS 顺序后屏幕已有显示，反馈触摸相对页面旋转 180°；当前映射改为 `x=raw_y, y=239-raw_x`（原始范围仍按 240×320）。修正后的四角、中心、边缘及交互精度仍待上板验证，颜色、亮度与启停电平未完成验收。

### GC2145 摄像头

| 参数 | 初始值 |
| --- | --- |
| 驱动 | espressif/esp32-camera，启用 CONFIG_GC2145_SUPPORT |
| SCCB 地址 / 预期 sensor PID | 7-bit 0x3C / 0x2145 |
| SCCB 总线 | 复用 I2C_NUM_0；支持该接口的版本使用 pin_sccb_sda=-1、sccb_i2c_port=0 |
| XCLK | GPIO5，20MHz（初始选择，取自乐鑫常规 camera 配置） |
| XCLK LEDC 资源 | LEDC_TIMER_0、LEDC_CHANNEL_0 |
| 像素格式 | PIXFORMAT_RGB565 |
| 分辨率 | FRAMESIZE_QVGA，320×240 |
| 帧缓冲 | CAMERA_FB_IN_PSRAM，fb_count=1，CAMERA_GRAB_WHEN_EMPTY |
| pin_pwdn / pin_reset | -1 / -1；PWDN 由 PCA9557 IO2 管理 |
| 镜像 / 翻转 | sensor 保留驱动默认；前置预览在 UI staging 拷贝时水平镜像，不翻转上下方向 |

GC2145 地址 / PID 依据 [乐鑫 sensor.h](https://github.com/espressif/esp32-camera/blob/master/driver/include/sensor.h)。支持开关依据 [乐鑫 camera Kconfig](https://github.com/espressif/esp32-camera/blob/master/Kconfig)。XCLK、缓冲配置的接口参照 [camera 官方说明](https://github.com/espressif/esp32-camera)及 [camera 配置结构](https://github.com/espressif/esp32-camera/blob/master/driver/include/esp_camera.h)。QVGA RGB565 一帧为 153600 字节，先以单缓冲完成采集和屏幕预览，再按吞吐需求考虑双缓冲。

GC2145 的乐鑫驱动实现 RGB565 / YUV422，sensor 表标记不支持原生 JPEG，因此不套用 OV2640 示例中的 PIXFORMAT_JPEG；需要 JPEG 文件时另做软件编码。该驱动的部分图像调节函数是空实现，不能仅凭通用 sensor API 存在就认为全部生效。依据 [GC2145 驱动](https://github.com/espressif/esp32-camera/blob/master/sensors/gc2145.c)及 [sensor 能力表](https://github.com/espressif/esp32-camera/blob/master/driver/sensor.c)。

启动流程：扩展器先保持 PWDN=1；开始初始化摄像头时，szpi_camera 通过 board 语义接口将其拉到 0，并等待 10ms，再调用 esp_camera_init()（驱动建立 XCLK 并执行传感器软件复位 / 寄存器初始化）。10ms 为参照 [乐鑫 camera 探测代码](https://github.com/espressif/esp32-camera/blob/v2.1.8/driver/esp_camera.c)选定的等待值，不是从原理图推导出的硬件最小时序。驱动会自行执行 GC2145 软件复位后的延时，不重复操作共用 RESET。停止时驱动 deinit 成功后再通过 board 接口断电。

### ES7210 / ES8311 音频

- 初始录音采用 **48kHz、16bit、双通道（MIC1/MIC2）、Philips 格式**；ES7210 先按本板官方例程的 **TDM 接收，slot0|slot1** 初始化，MCLK 为 256×采样率，即 **12.288MHz**。ESP32 提供时钟，音频芯片使用从机方式。依据 [立创 ES7210 官方例程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/audio-input-es7210.html)。
- 初始播放选择同样的 48kHz、16bit、两槽标准 Philips I²S；ES8311 为单声道 DAC，先将单声道样本复制到左右两槽，明确驱动使用的 DAC 槽。此播放方案是工程初始选择，需由 Codec 驱动同步设置格式和时钟。
- 音频控制器选择 I2S_NUM_0，优先统一分配 TX/RX 资源。共享 MCLK=38、BCLK=14、WS=13 只能由一个时钟源驱动；初始化 TX/RX 时帧周期和总位数必须匹配。ESP-IDF 6.1 支持符合相同帧时序条件的 STD/TDM 配对，参见 [ESP-IDF I²S 全双工说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)。
- 初始先分别验证录音与播放，再启用同时收发。第三路 MIC3 回采用于后续 AEC；启用三路时需要重新设计 ES7210 输出槽和播放帧时序，不能只给双通道配置追加一个接收槽。
- 启动时 PA_EN=0；Codec、时钟及有效数据流准备好后 PA_EN=1。停止播放先关闭功放，再停数据 / 时钟。高开启、低关闭依据 [立创 MP3 官方例程](https://wiki.lckfb.com/zh-hans/szpi-esp32s3/beginner/mp3.html)。

初版代码已采用 esp_codec_dev 1.6.2 和 board 新 I²C bus；AUDIO 页提供麦克风输入增益（0–36dB，默认 18dB）和扬声器输出音量（0–100%，默认 70%）滑条。调节通过 audio service 应用到活动采集 / 播放或下次启动。输出启动时先配置 codec 与音量，首个 PCM 写入前才通过 board 语义接口开启 PA，停止 / 写失败时关闭 PA；测试音峰值为 12000/32767。增益范围和听感仍需实物确认。ES7210 RX TDM、ES8311 TX STD 的实际 WS/BCLK 槽时序与同时收发仍未实测，不能视为已校准。AUDIO 和 SD CARD 验证入口分为独立页面；软件构建和未完成项见[第四阶段验证](develop/phase_4_validation.md)。

### 上板时的一次性校准

以上默认值足够开始编写板级驱动。首次烧录验证 PSRAM、I²C 应答、屏幕四色 / 四角、触摸映射、GC2145 PID 与帧数据、双麦克风录音及喇叭播放。实际修正只集中在显示 / 触摸 / 摄像头方向、总线稳定频率和音频槽映射，并回写本文件；不再把这些常规初始参数作为开工前的用户确认项。

## 后续编码约定

- 板级 GPIO、扩展器 bit 编号、I²C 地址集中定义，避免在各驱动内重复硬编码；主控 GPIO 和扩展器端口分开命名。
- GPIO0、3、45、46 为启动配置脚；它们在本板已有明确用途，外接电路应保持启动时所需电平。
- GPIO35/36/37 虽然原理图外部为 NC，但用于 N16R8 的 Octal PSRAM；不要重新配置。
- GPIO10/11 是本原理图中明确引到多用外部接口的通用 IO；其他已接外设的引脚只有在相应外设停用且电气连接允许时才考虑复用。
- 触摸与 IMU 采用轮询，TF 卡没有 CD 检测 GPIO；LCD / 摄像头没有独立复位 GPIO。
- 先建立共享 I²C 总线及扩展器控制，再初始化依赖它们的屏幕、摄像头与功放；实际有效电平、时序及外设型号按器件手册和实物验证。

LCD 空白屏诊断：`CONFIG_SZPI_DISPLAY_SPI_CLOCK_HZ` 可覆盖板级默认 SPI 频率（0 表示使用默认 20MHz），`CONFIG_SZPI_DISPLAY_SPI_MODE` 默认 2。诊断设置不代表已完成面板校准，进展见 [第二阶段验证](develop/phase_2_validation.md)。

高刷新配置：LVGL `LV_DEF_REFR_PERIOD=16`ms，UI 调度 4ms，FreeRTOS tick 1000Hz；两个内部 DMA 缓冲各 40 行，共 51,200 字节。80MHz 全屏 RGB565 纯像素发送约 15.36ms，不含命令、调度与绘制开销；约 60Hz 为配置目标，实际帧率和高速信号稳定性仍需上板验证。触摸轮询保持 20ms。
