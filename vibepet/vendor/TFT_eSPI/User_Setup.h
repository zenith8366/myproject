// ============================================================
// TFT_eSPI User_Setup.h  ——  VibePet 仓库内自带副本
//
// ⚠️ 这是 vibepet 仓库自带的 TFT_eSPI（vendor 目录），不是 Arduino 库目录
//    里那份。本文件的引脚取值以设计文档 §4.7.2（「接线的唯一权威依据」）
//    为准，与 firmware/TFT_eSPI_User_Setup.h 模板保持一致。
//
//    编译时必须用 arduino-cli --libraries vendor 指向本目录，否则会用到
//    Arduino 库目录里那份未修正的 TFT_eSPI。
// ============================================================


// ============================================================
// 1. 驱动与分辨率
// ============================================================

// 1.77" 模块用的驱动 IC 是 ST7735S，与 ST7735 同族，宏名沿用 ST7735_DRIVER
#define ST7735_DRIVER

// 屏幕原生分辨率 128(宽)×160(高)；固件用 setRotation(1) 转成横屏 160×128
#define TFT_WIDTH  128
#define TFT_HEIGHT 160

// 初始化序列（「色带 / tab」变体）—— 只能开一个。
// BLACKTAB 依据 test-firmware/tfttest_esp32c3 那份实测可用的代码
// （它用的是 Adafruit 的 INITR_BLACKTAB，与此对应）。
// 若出现花屏 / 偏色 / 显示区域偏移，按下面顺序逐个换着试：
//   ST7735_INITB → ST7735_GREENTAB → ST7735_GREENTAB2 →
//   ST7735_GREENTAB3 → ST7735_REDTAB → ST7735_BLACKTAB
#define ST7735_BLACKTAB


// ============================================================
// 2. 引脚接线（2026-09-23 起用这一版）
// ============================================================
//
// 模块丝印对不上时对照这里：
//    A0 / DC / RS  → 同一根脚，数据/命令切换，接 GPIO 0
//    RES / RST     → 同一根脚，复位，接 GPIO 5
//    SDA / MOSI    → 数据，接 GPIO 6
//    SCK / SCL     → 时钟，接 GPIO 4
//
// 选脚依据：
//   · SCK/MOSI/CS 与 test-firmware/tfttest_esp32c3（本机实测点亮过的那份）
//     完全一致，保持已验证的部分不变。
//   · DC 从设计文档原本的 GPIO8 改成 GPIO0：GPIO8 是 ESP32-C3 的 strapping
//     启动脚，能不占就不占；GPIO0 在 C3 上是普通脚，不参与启动模式选择。
//   · 成功配置里 DC/RES 用的是 GPIO1/GPIO2，那两个脚在本项目被批准按钮和
//     状态 LED 占了，必须让开。
//
// ⚠️ 别和这几个撞：GPIO1=批准按钮、GPIO2=状态LED、GPIO3=蜂鸣器、
//    GPIO10=拒绝按钮、GPIO9=启动模式脚（碰了就进下载模式）、
//    GPIO8=strapping 脚。
#define TFT_CS    7   // 片选
#define TFT_DC    0   // 数据/命令切换（模块上标 A0 / DC / RS）
#define TFT_RST   5   // 复位
#define TFT_MOSI  6   // 数据（模块上标 SDA）
#define TFT_SCLK  4   // 时钟

// 背光（模块上的 LED / BL / LEDA 脚）直接接 3.3V 常亮，不占用 GPIO，
// 因此这里不定义 TFT_BL。
// #define TFT_BL   0            // ← 换成真正的空闲引脚号
// #define TFT_BACKLIGHT_ON HIGH

// 少数模块的红蓝通道是反的（画面整体偏蓝或偏红），颜色不对就取消注释
// #define TFT_RGB_ORDER TFT_BGR

// 如果画面是反色（白变黑），取消这行注释
// #define TFT_INVERSION_ON


// ============================================================
// 3. 字体
// ============================================================
//
// GLCD 是内置的 6×8 等宽字体，固件的标题类文字（IDLE / WORKING / APPROVE? …）
// 全部通过 drawCentered() 里的 setTextSize() 用它。
// 固件未使用 GFXFF 与平滑字体，故不加载这两项，省 Flash。
#define LOAD_GLCD

// Font 2 (16px) 与 Font 4 (26px) 供后续扩展使用，不影响当前固件。
#define LOAD_FONT2
#define LOAD_FONT4

// 注意：以上字体都只包含 ASCII 字形。正文里的中文由 U8g2_for_TFT_eSPI 的
// wqy12 GB2312 字体渲染（见 VibePet.ino 的 FONT_BODY），与本文件无关；
// 但需要额外安装 U8g2 与 U8g2_for_TFT_eSPI 两个库，且分区要选 Huge APP
// （字库约 200 KB）。


// ============================================================
// 4. SPI 速度
// ============================================================
//
// 先用保守的 10 MHz 把链路跑通。屏幕稳定显示后可以逐步往上提，
// 提到 27000000 仍正常的话就用它（画面刷新更快）。
// 若出现条纹、噪点或偶发花屏，就往下降。
#define SPI_FREQUENCY  10000000

// ESP32-C3 的 SPI 读写不需要单独的读频率设置
