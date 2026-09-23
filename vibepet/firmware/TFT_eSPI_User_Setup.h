// ════════════════════════════════════════════════════════════════════════════
//  VibePet 的 TFT_eSPI 配置参考
//
//  这不是编译单元，也不是「覆盖文件」了 —— 编译时实际生效的是仓库自带的
//  那份 TFT_eSPI 副本：
//
//      vendor/TFT_eSPI/User_Setup.h        ← 实际生效的就是它
//
//  本文件只作为「这一版配置长什么样、为什么这么选」的可读参考，方便你对照
//  检查 vendor 里的值有没有被改乱。**改配置请改 vendor 里那份**，本文件跟着
//  同步留档即可。
//
//  ── 为什么改成仓库自带一份，而不是覆盖 Arduino 库 ──────────────────────
//
//  TFT_eSPI 2.5.43 在 ESP32 核心 3.x（ESP-IDF 5.x）上有两处会直接导致
//  固件跑不起来的兼容性问题，必须在库源码里修：
//
//    1. Processors/TFT_eSPI_ESP32_C3.h 的 REG_SPI_BASE 被核心的兼容垫片顶掉，
//       寄存器基址算成 0，_spi_user 指针落到 0x10，一写就崩溃重启。
//    2. 同一个文件里把 TFT_MISO 强制设成 TFT_MOSI，导致这两个信号撞在同一个
//       GPIO 上，MOSI 挂载失败，SPI 发不出数据，卡死在 tft.init() 里。
//
//  与其去改 Arduino 库目录（重装库或换电脑就丢），不如把整份库自带进仓库、
//  在副本里修。详见 vendor/TFT_eSPI/Processors/TFT_eSPI_ESP32_C3.h 的注释。
//
//  编译方式见 CLAUDE.md「常用命令」——关键是 --libraries vendor。
// ════════════════════════════════════════════════════════════════════════════

// ─────────────────────────── 驱动与分辨率 ───────────────────────────
// 1.77" 模块用的驱动 IC 是 ST7735S，与 ST7735 同族，宏名沿用 ST7735_DRIVER。
#define ST7735_DRIVER

// 屏幕原生分辨率是 128(宽)×160(高)。固件里用 setRotation(1) 转成横屏 160×128。
#define TFT_WIDTH  128
#define TFT_HEIGHT 160

// ─────────────────────────── 引脚接线 ───────────────────────────
// 对应设计文档 4.7.2 的接线表。ESP32-C3 的 SPI 可以走任意 GPIO（GPIO 矩阵），
// 所以直接用 TFT_CS / TFT_DC 这种写法，而不是指定 VSPI/HSPI。
//
// 选脚依据（2026-09-23 定稿）：
//   · SCK / MOSI / CS 与 test-firmware/tfttest_esp32c3（本机实测点亮过的
//     那份代码）完全一致，已验证的部分保持不动。
//   · DC 用 GPIO0 而**不是** GPIO8：GPIO8 是 ESP32-C3 的 strapping 启动脚，
//     能不占就不占；GPIO0 在 C3 上是普通脚，不参与启动模式选择。
//   · 那份成功代码里 DC/RES 用的是 GPIO1/GPIO2，这两个脚在本项目被批准按钮
//     和状态 LED 占了，必须让开。
#define TFT_CS   7   // 片选
#define TFT_DC   0   // 数据/命令切换（模块上标 A0 / DC / RS 都指这根）
#define TFT_RST  5   // 复位
#define TFT_MOSI 6   // 数据（模块上标 SDA）
#define TFT_SCLK 4   // 时钟

// 背光（模块上的 LED / BL / LEDA 脚）直接接 3.3V 常亮，不占用 GPIO，因此这里不定义 TFT_BL。
// ⚠️ 若你的模块背光需要软件控制：本项目的 GPIO 已全部被 TFT / 按钮 / 蜂鸣器 /
// 状态 LED 占用（见设计文档 4.7 的引脚占用总览）。下面示例曾写 GPIO 3，而
// GPIO 3 现在是蜂鸣器 —— 照抄会和蜂鸣器抢引脚（两者都用 LEDC），改成空闲脚再用。
// #define TFT_BL   0            // ← 换成真正的空闲引脚号
// #define TFT_BACKLIGHT_ON HIGH

// ─────────────────────────── 初始化序列（最可能需要改的地方）───────────────────────────
// 不同厂商的 1.77" ST7735S 模块出厂初始化参数不同，选错会表现为：
//   花屏 / 偏色 / 显示区域偏移一格 / 四周有噪点。
//
// 当前用的是 BLACKTAB —— 依据 test-firmware/tfttest_esp32c3 那份在本机实测
// 显示成功的代码（它用的是 Adafruit 的 INITR_BLACKTAB，与此对应）。
// 若画面不正常，按下面顺序逐个换着试：
//
//   #define ST7735_INITB
//   #define ST7735_GREENTAB
//   #define ST7735_GREENTAB2
//   #define ST7735_GREENTAB3
//   #define ST7735_REDTAB
//   #define ST7735_BLACKTAB     ← 当前值
//
// 注意：这几个宏只能开一个。
#define ST7735_BLACKTAB

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
// 先用保守的 10 MHz 把链路跑通。屏幕稳定显示后可以逐步往上提，
// 提到 27000000 仍正常的话就用它（画面刷新更快）。
// 若出现条纹、噪点或偶发花屏，就往下降。
#define SPI_FREQUENCY  10000000

// ESP32-C3 的 SPI 读写不需要单独的读频率设置
