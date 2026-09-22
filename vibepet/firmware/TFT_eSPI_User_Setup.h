// ════════════════════════════════════════════════════════════════════════════
//  VibePet 的 TFT_eSPI 配置模板
//
//  这不是编译单元，而是一份「覆盖文件」。TFT_eSPI 的引脚与屏幕参数不走代码，
//  全部在库自己的 User_Setup.h 里配置，所以：
//
//    把本文件的内容整体复制，覆盖掉 TFT_eSPI 库中的 User_Setup.h。
//
//  库文件位置（Arduino IDE 默认）：
//    Windows : 我的文档/Arduino/libraries/TFT_eSPI/User_Setup.h
//    macOS   : ~/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
//    Linux   : ~/Arduino/libraries/TFT_eSPI/User_Setup.h
//
//  ⚠️ 覆盖前先备份原文件（设计文档 8.3 的第 5 条应对措施）。
// ════════════════════════════════════════════════════════════════════════════

// ─────────────────────────── 驱动与分辨率 ───────────────────────────
// 1.77" 模块用的驱动 IC 是 ST7735S，与 ST7735 同族，宏名沿用 ST7735_DRIVER。
#define ST7735_DRIVER

// 屏幕原生分辨率是 128(宽)×160(高)。固件里用 setRotation(1) 转成横屏 160×128。
#define TFT_WIDTH  128
#define TFT_HEIGHT 160

// ─────────────────────────── 引脚接线 ───────────────────────────
// 对应设计文档 4.2 的接线表。ESP32-C3 的 SPI 可以走任意 GPIO（GPIO 矩阵），
// 所以直接用 TFT_CS / TFT_DC 这种写法，而不是指定 VSPI/HSPI。
#define TFT_CS   7   // 片选
#define TFT_DC   8   // 数据/命令切换（模块上标 A0）
#define TFT_RST  5   // 复位
#define TFT_MOSI 6   // 数据（模块上标 SDA）
#define TFT_SCLK 4   // 时钟

// 背光（模块上的 LED 脚）直接接 3.3V 常亮，不占用 GPIO，因此这里不定义 TFT_BL。
// 若你的模块背光需要控制，取消下面两行的注释并接到实际引脚。
// #define TFT_BL   3
// #define TFT_BACKLIGHT_ON HIGH

// ─────────────────────────── 初始化序列（最可能需要改的地方）───────────────────────────
// 不同厂商的 1.77" ST7735S 模块出厂初始化参数不同，选错会表现为：
//   花屏 / 偏色 / 显示区域偏移一格 / 四周有噪点。
// 逐个试，试到画面正常为止：
//
//   #define ST7735_INITB        ← 先试这个
//   #define ST7735_GREENTAB
//   #define ST7735_GREENTAB2
//   #define ST7735_GREENTAB3
//   #define ST7735_REDTAB
//   #define ST7735_BLACKTAB
//
// 注意：这几个宏只能开一个。
#define ST7735_INITB

// 少数模块的红蓝通道是反的（画面整体偏蓝或偏红）。若颜色不对，
// 取消下面这行的注释换一下 R/B 顺序。
// #define TFT_RGB_ORDER TFT_BGR

// 如果画面是反色（白变黑），取消这行注释
// #define TFT_INVERSION_ON

// ─────────────────────────── 字体 ───────────────────────────
// GLCD 是内置的 6×8 等宽字体，固件的标题类文字（IDLE / WORKING / APPROVE? …）用它。
#define LOAD_GLCD

// Font 2 (16px) 与 Font 4 (26px) 供后续扩展使用，不影响当前固件。
#define LOAD_FONT2
#define LOAD_FONT4

// 注意：以上字体都只包含 ASCII 字形。正文（命令摘要 / 错误信息 / 状态副标题）
// 里的中文由 U8g2_for_TFT_eSPI 的 wqy12 GB2312 字体渲染（见 VibePet.ino 的
// FONT_BODY），与本文件无关；但需要额外安装 U8g2 与 U8g2_for_TFT_eSPI 两个库，
// 且分区要选 Huge APP（字库约 200 KB）。

// ─────────────────────────── SPI 速度 ───────────────────────────
// 27 MHz 对 ST7735 是稳妥值。杜邦线接线较长、画面出现条纹或噪点时，
// 降到 15000000 试试。
#define SPI_FREQUENCY  27000000

// ESP32-C3 的 SPI 读写不需要单独的读频率设置
