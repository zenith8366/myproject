/* ============================================================================
 * VibePet —— AI 编程助手物理状态显示与审批终端（Arduino UNO R3 · USB 有线版）
 * ============================================================================
 *
 * ━━━━━━━━━━━━━━━ 新手导读：这个程序在干嘛 ━━━━━━━━━━━━━━━
 *
 * 桌上有块小屏幕，屏幕下方有两个按钮。电脑上的 Claude Code 每做完一件事、
 * 或者想执行一条危险命令之前，都会通过 USB 线告诉这块板子一句话；板子把
 * 它显示出来。需要你批准的时候，它会响一声、屏幕变黄，你按一下按钮，
 * 板子再把「批准 / 拒绝」发回电脑。
 *
 * 程序从头到尾就干三件事，全在一个循环里转（没有多任务、没有中断回调）：
 *
 *   ① 收：把串口来的字节攒成一条完整的消息（一条消息 = 一行，以换行结尾）
 *   ② 看：这条消息是什么意思？（心跳？切换状态？要我审批？）
 *   ③ 显示 + 按键：把当前状态画到屏幕上；顺手看看有没有人按按钮
 *
 * ━━━━━━━━━━━━━━━ 三件必须先知道的事 ━━━━━━━━━━━━━━━
 *
 * 【一】这根 USB 线既是数据通道，也是烧录口。
 *   所以程序**不能往串口打印调试信息** —— 那些字会被电脑当成协议报文，
 *   轻则看不懂、重则解析失败。要调试就把下面这个开关打开：
 *
 *       #define DEBUG_SERIAL 1     // 打开后日志混在链路上，只适合脱离 daemon 单独调
 *
 * 【二】这块板子只有 2KB 内存、32KB 存储（是 ESP32 的两百分之一）。
 *   所以有几样东西是这个项目**明令禁止**的，看到别奇怪：
 *     · 不能用 String 类        —— 它每次拼接都可能向系统要内存，很快把 2KB 撕碎
 *     · 不能用 ArduinoJson 之类的 JSON 库 —— 太占存储和内存，见 vibepet_proto.h
 *     · 不能用 float / sin() / sqrt()  —— 这芯片没有小数运算单元，一用就要拖进一大坨代码
 *     · 字符串字面量必须包 F("...") —— 否则那些字会被从存储搬进本来就不够用的内存
 *
 * 【三】主循环里**任何一处连续卡顿都不许超过 3 毫秒**。
 *   因为串口在不停来数据，而芯片的接收缓冲只有 256 字节（约 22 毫秒就填满）。
 *   一旦某段代码埋头画屏幕超过这个时间，后面来的字节就被冲掉 —— 一条消息
 *   只要缺了几个字节，整条就废了。所以：
 *     · 整屏重绘要**切片**（画几行就停下来收一次串口）
 *     · 蜂鸣器要**非阻塞**（老版本用 delay 等响完，在这里会直接丢消息）
 *   这两处的代码里都有注释说明。
 *
 * ━━━━━━━━━━━━━━━ 屏幕长什么样（160×128 横屏）━━━━━━━━━━━━━━━
 *
 *      +------------------------------------------+  ← 闪烁边框（等待审批时）
 *      |                 APPROVE?                 |  ← 标题：内置字体的两倍大
 *      |                                          |
 *      |                 (图标区域)                |  ← 呼吸圆点 / 跑动方块 / 对勾 / 叉
 *      |                                          |
 *      |          Bash: rm -rf /tmp/build         |  ← 正文：12 像素中文字库，自动折行
 *      |          最多四行，一行放不下就用 .. 收尾       |
 *      +------------------------------------------+
 *
 * ━━━━━━━━━━━━━━━ 和设计文档的关系 ━━━━━━━━━━━━━━━
 *
 * 设计文档（仓库根目录那份 v3.0）是唯一权威规格，本文件是它的实现。
 * 文档 5.5 节的代码是**示意片段**，不能照抄；这里的才是能跑的。
 * 中文点阵字库是同目录的 cn_font.h（由 tools/gen_cn_font.py 生成），
 * 协议解析在 vibepet_proto.h。
 * ==========================================================================*/

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#include "cn_font.h"
#include "vibepet_proto.h"

// ═══════════════════════════ 一、配置区 ═══════════════════════════
//
// 想改行为就先看这一节。引脚为什么这么分，见设计文档 4.7。

// ── 引脚 ──
// 屏幕用硬件 SPI：SCK 固定 D13、MOSI 固定 D11，这两个改不了；
// CS / DC / RES 可以自己挑（这里与 test-firmware/tfttest_uno 一致）。
#define PIN_TFT_CS    10
#define PIN_TFT_DC     9
#define PIN_TFT_RST    8

#define PIN_BTN_APPROVE 2   // 批准按钮：一端接此脚，另一端接 GND
#define PIN_BTN_DENY    3   // 拒绝按钮：同上
#define PIN_BUZZER      4   // 无源蜂鸣器：靠方波驱动，空闲保持低
#define PIN_LED_STATUS  5   // 链路指示灯：心跳正常时点亮

// D0 / D1 是 USB 串口，**永远不要往上接外设**（接了电脑就认不到设备）。

// ── 调试开关 ──
// 打开后会把日志打进串口。**只在没有跑 daemon、单独用串口监视器调试时打开** ——
// 正常使用时打开会把日志混进协议报文里。
#define DEBUG_SERIAL 0

// ── 蜂鸣器 ──
// 0 = 无源（本项目用的，靠方波发声）；1 = 有源（给个电平就响）
#define BUZZER_ACTIVE   0
#define BUZZER_TONE_HZ  2700   // 无源蜂鸣器的发声频率

// ── 协议 ──
#define MSG_MAX   160   // 正文文案的字节上限（与 daemon 的约定一致）
#define RID_MAX     8   // request_id 的长度（daemon 生成的是 uuid 前 8 位）

// ── 时序（毫秒）──
#define WATCHDOG_TIMEOUT 5000   // 超过这么久没收到任何消息 → 显示「失联」
#define DEBOUNCE_MS       200   // 按钮去抖窗口
#define ANIM_FRAME_MS      90   // 动画换帧间隔
#define BLINK_MS          500   // 等待审批时边框闪烁的间隔

// ── 屏幕布局（横屏画布 160 宽 × 128 高）──
#define SCREEN_W   160
#define SCREEN_H   128
#define TITLE_X     0
#define TITLE_Y     4
#define ICON_CX    80    // 图标区中心
#define ICON_CY    40
#define ICON_HALF  24    // 图标区半边长（清理时按这个范围擦）
#define BODY_TOP   68    // 正文第一行的顶边
#define BODY_LINE_H 15   // 正文行高（字高 13 + 2 行距）
#define BODY_LINES   4   // 正文最多几行

// ── 颜色（RGB565）──
// 背景纯黑是为了省电和对比度；六种状态各有自己的颜色（设计文档 3.3 的表）。
#define C_BG      ST77XX_BLACK
#define C_TITLE   ST77XX_WHITE
#define C_BODY    ST77XX_WHITE
#define C_IDLE    ST77XX_WHITE
#define C_WORKING ST77XX_GREEN
#define C_NEEDS   ST77XX_YELLOW
#define C_DONE    ST77XX_CYAN
#define C_ERROR   ST77XX_RED
#define C_LOST    ST77XX_ORANGE

// 把颜色整体调暗（每个通道右移一位）。用来画「不活跃」的元素，省得再多定义几种颜色。
#define DIM(c) ((uint16_t)(((c) >> 1) & 0x7BEF))

// ═══════════════════════════ 二、全局状态 ═══════════════════════════

// 六种状态（设计文档 3.3）。LOST 是设备自己判断的，其余五种来自电脑端。
enum PetState { ST_IDLE, ST_WORKING, ST_NEEDS_YOU, ST_DONE, ST_ERROR, ST_LOST };

static Adafruit_ST7735 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

static VpLine rxLine;              // 串口行缓冲（收字节用，见 vibepet_proto.h）
static char msgBuf[MSG_MAX + 1];   // 当前要显示的正文（摘要 / 错误信息 / 副标题）
static char ridBuf[RID_MAX + 1];   // 正在等待审批的 request_id；空串表示没有

static PetState currentState = ST_IDLE;
static PetState lastValidState = ST_IDLE;  // 失联恢复时要回到的那个状态

static unsigned long lastHeartbeat = 0;    // 上次收到**任何**合法消息的时刻
static unsigned long lastApproveMs = 0;
static unsigned long lastDenyMs = 0;
static unsigned long lastAnimMs = 0;
static unsigned long lastBlinkMs = 0;
static uint8_t animPhase = 0;
static bool borderOn = false;               // 等待审批时边框的亮灭

// 放在存储（Flash）里的中文常量。用 strcpy_P 取出来才能用 —— 直接写 "已批准"
// 会把它塞进内存，而内存只有 2KB。
static const char STR_READY[]    PROGMEM = "就绪";
static const char STR_APPROVED[] PROGMEM = "已批准";
static const char STR_DENIED[]   PROGMEM = "已拒绝";

// ═══════════════════════════ 三、串口泵 ═══════════════════════════

// 把串口里已经到达的字节搬进行缓冲。**只搬不处理** —— 这样在画屏幕的中途
// 也能安全调用（见 vibepet_proto.h 里关于「收和分要分开」的说明）。
static void pumpSerial() {
  while (Serial.available() > 0) {
    vpLineFeed(&rxLine, (char)Serial.read());
  }
}

#if DEBUG_SERIAL
// 调试日志。只在 DEBUG_SERIAL 打开时才有内容，正常使用时是空的。
static void dbg(const __FlashStringHelper *text) {
  Serial.print(F("# "));
  Serial.println(text);
}
#else
#define dbg(text) do {} while (0)
#endif

// ═══════════════════════════ 四、画字（中英文共用一套）═══════════════════════════

// 取字形某一行的点阵：cnFontRow() 在 cn_font.h 里（由字库生成工具一并生成）。
// 放在那儿是为了让电脑上的离线测试能验证同一份代码 —— 这里曾经踩过一个坑：
// 参数写成 8 位整数，导致索引超过 255 的字全被截断成别的字（屏幕上表现为
// 「有的汉字变成英文字母」）。详见 cn_font.h 里那段注释。

// 从 UTF-8 字节流里取下一个字，指针前移。返回 0 表示到头了。
//
// 一个汉字在 UTF-8 里占 3 个字节，所以**绝不能按字节数当字数用** ——
// 这也是为什么折行、算宽度都必须走这个函数。
static uint16_t utf8Next(const char **cursor) {
  const uint8_t *p = (const uint8_t *)*cursor;
  uint8_t lead = *p;
  if (lead == 0) return 0;

  uint8_t length;
  uint16_t code;
  if (lead < 0x80) {
    length = 1;
    code = lead;
  } else if ((lead & 0xE0) == 0xC0) {
    length = 2;
    code = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    code = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    code = lead & 0x07;
  } else {
    // 不是合法的首字节（可能收到了半截数据），跳过它免得卡死
    (*cursor)++;
    return utf8Next(cursor);
  }

  for (uint8_t i = 1; i < length; i++) {
    if ((p[i] & 0xC0) != 0x80) {   // 后面接的不是续接字节 → 这条序列是坏的
      (*cursor)++;
      return utf8Next(cursor);
    }
    code = (uint16_t)((code << 6) | (p[i] & 0x3F));
  }
  *cursor += length;
  return code;
}

// 取某个字的显示宽度（像素）。汉字是 12，英文标点是 5~8。
static uint8_t glyphAdvance(uint16_t code) {
  int16_t glyph = cnFontFind(code);
  if (glyph < 0) return CN_FONT_W;   // 没收录的字画成方框，按满宽算
  return pgm_read_byte(&cn_font_adv[glyph]);
}

// 算一段 UTF-8 文本的像素宽度
static uint16_t textWidth(const char *text) {
  uint16_t width = 0;
  while (*text != '\0') {
    uint16_t code = utf8Next(&text);
    if (code == 0) break;
    width += glyphAdvance(code);
  }
  return width;
}

// 画一个字。没收录的字画一个空心方框 —— 一眼能看出「少了个字」，
// 比显示乱码或者什么都不显示都更有用。
//
// 为什么要先把一行像素拼进一个小数组再一次性推给屏幕？因为屏幕每次写
// 都要发一串命令（设置窗口、切数据/命令线），逐像素写的话，光是命令开销
// 就比像素本身还多。按行推一次，一个汉字只要 13 次。
static void drawGlyph(int16_t x, int16_t top, uint16_t code, uint16_t fg, uint16_t bg) {
  int16_t glyph = cnFontFind(code);
  uint8_t width = glyphAdvance(code);
  uint16_t pixels[CN_FONT_W];   // 一行像素（栈上 24 字节）

  tft.startWrite();
  tft.setAddrWindow(x, top, width, CN_FONT_H);
  for (uint8_t row = 0; row < CN_FONT_H; row++) {
    uint16_t mask;
    if (glyph < 0) {
      // 空心方框：只画边框
      bool edge = (row == 0 || row == CN_FONT_H - 1);
      mask = 0;
      for (uint8_t c = 0; c < CN_FONT_W; c++) {
        bool on = edge || c == 0 || c == CN_FONT_W - 1;
        if (on && c < width) mask |= (uint16_t)(1 << c);
      }
    } else {
      mask = cnFontRow((uint16_t)glyph, row);
    }
    // 位序：mask 的第 c 位就是第 c 列（位 0 = 最左）。别写成 0x800 >> c ——
    // 那样读到的是镜像的列，整个字会左右翻转。
    for (uint8_t c = 0; c < width; c++) {
      pixels[c] = (mask & (1 << c)) ? fg : bg;
    }
    tft.writePixels(pixels, width);
  }
  tft.endWrite();
  pumpSerial();   // 画完一个字就收一次串口，别让缓冲溢出来
}

// 画一整行文本（不做折行，从左边 x 开始）。返回画完后的 x。
static int16_t drawText(int16_t x, int16_t top, const char *text, uint16_t fg, uint16_t bg) {
  int16_t cursor = x;
  while (*text != '\0') {
    const char *before = text;
    uint16_t code = utf8Next(&text);
    if (code == 0) break;
    (void)before;
    drawGlyph(cursor, top, code, fg, bg);
    cursor += glyphAdvance(code);
  }
  return cursor;
}

// 画居中的一整行（先量宽度再定起点）
static void drawTextCentered(int16_t top, const char *text, uint16_t fg, uint16_t bg) {
  uint16_t width = textWidth(text);
  if (width > SCREEN_W) width = SCREEN_W;
  drawText((SCREEN_W - width) / 2, top, text, fg, bg);
}

// 按屏宽折行，最多 BODY_LINES 行，每行水平居中；放不下时最后一行用 ".." 收尾。
//
// 折行必须**按字**而不是按字节：一个汉字 3 字节，从中间切开就会出乱码。
// 所以这里先扫一遍算出每一行从哪个字节开始，再逐行画。
static void drawBodyText(const char *text) {
  const char *lineStart[BODY_LINES];
  uint16_t lineWidth[BODY_LINES];
  uint8_t lineCount = 0;

  const char *cursor = text;
  const char *lineBegin = text;
  uint16_t width = 0;

  while (*cursor != '\0' && lineCount < BODY_LINES) {
    const char *before = cursor;
    uint16_t code = utf8Next(&cursor);
    if (code == 0) break;
    uint8_t advance = glyphAdvance(code);
    if (width + advance > SCREEN_W) {
      // 这一行放不下了：记录下来，另起一行
      lineStart[lineCount] = lineBegin;
      lineWidth[lineCount] = width;
      lineCount++;
      lineBegin = before;
      width = advance;
    } else {
      width += advance;
    }
  }
  if (lineCount < BODY_LINES && lineBegin < cursor) {
    lineStart[lineCount] = lineBegin;
    lineWidth[lineCount] = width;
    lineCount++;
  }

  for (uint8_t i = 0; i < lineCount; i++) {
    int16_t x = (int16_t)((SCREEN_W - lineWidth[i]) / 2);
    int16_t top = BODY_TOP + i * BODY_LINE_H;
    // 最后一行如果还有剩，末尾补 ".." 表示被截断了
    const char *end = (i + 1 < lineCount) ? lineStart[i + 1] : cursor;
    const char *p = lineStart[i];
    bool truncated = (i + 1 == lineCount) && (*cursor != '\0');
    while (p < end) {
      uint16_t code = utf8Next(&p);
      if (code == 0) break;
      if (truncated && x + glyphAdvance(code) + 18 > SCREEN_W) break;
      drawGlyph(x, top, code, C_BODY, C_BG);
      x += glyphAdvance(code);
    }
    if (truncated && x + 12 <= SCREEN_W) {
      x = drawText(x, top, "..", C_BODY, C_BG);
    }
    pumpSerial();
  }
}

// ═══════════════════════════ 五、切片绘制 ═══════════════════════════
//
// 屏幕整屏有 160×128 = 两万个像素，一次性填满要几十毫秒 —— 远超 3 毫秒的
// 纪律（见文件头的说明）。所以凡是「大面积」的绘制都切成小片，片与片之间
// 收一次串口。

#define SLICE_H 8   // 每片 8 行：约 2.5 毫秒，安全

static void fillScreenSliced(uint16_t color) {
  for (int16_t y = 0; y < SCREEN_H; y += SLICE_H) {
    tft.fillRect(0, y, SCREEN_W, SLICE_H, color);
    pumpSerial();
  }
}

// 清空图标区（也切片，虽然 48×48 只有 4 毫秒出头，但没必要冒险）
static void clearIconArea() {
  for (int8_t y = -ICON_HALF; y < ICON_HALF; y += 8) {
    tft.fillRect(ICON_CX - ICON_HALF, ICON_CY + y, ICON_HALF * 2, 8, C_BG);
    pumpSerial();
  }
}

// 画一条有粗细的线（内置库只能画 1 像素宽，这里并排画几条凑粗）
static void thickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      uint8_t weight, uint16_t color) {
  int8_t half = (int8_t)(weight / 2);
  for (int8_t i = -half; i <= half; i++) {
    tft.drawLine(x0 + i, y0, x1 + i, y1, color);
  }
}

// ═══════════════════════════ 六、六种状态的图标 ═══════════════════════════

// 空闲：一个会「呼吸」的圆点（半径按三角波来回变，不用 sin —— 这芯片算不动小数）
static void drawIdleIcon() {
  static uint8_t lastRadius = 0;
  uint8_t phase = animPhase & 0x0F;
  uint8_t step = (phase < 8) ? phase : (uint8_t)(15 - phase);
  uint8_t radius = (uint8_t)(6 + step);

  if (lastRadius > 0) tft.fillCircle(ICON_CX, ICON_CY, lastRadius, C_BG);
  tft.fillCircle(ICON_CX, ICON_CY, radius, C_IDLE);
  lastRadius = radius;
}

// 工作中：五个方块像跑马灯一样依次点亮
#define WORK_DOTS 5
static void drawWorkingIcon() {
  int16_t x0 = ICON_CX - (WORK_DOTS * 10 - 2) / 2;
  uint8_t lit = (uint8_t)(animPhase % WORK_DOTS);
  for (uint8_t i = 0; i < WORK_DOTS; i++) {
    tft.fillRect(x0 + i * 10, ICON_CY - 4, 8, 8,
                 (i == lit) ? C_WORKING : DIM(C_WORKING));
  }
  pumpSerial();
}

// 完成：一个对勾
static void drawDoneIcon() {
  thickLine(ICON_CX - 12, ICON_CY, ICON_CX - 3, ICON_CY + 9, 3, C_DONE);
  thickLine(ICON_CX - 4, ICON_CY + 9, ICON_CX + 12, ICON_CY - 9, 3, C_DONE);
}

// 出错：一个叉
static void drawErrorIcon() {
  thickLine(ICON_CX - 10, ICON_CY - 10, ICON_CX + 10, ICON_CY + 10, 3, C_ERROR);
  thickLine(ICON_CX + 10, ICON_CY - 10, ICON_CX - 10, ICON_CY + 10, 3, C_ERROR);
}

// 等待审批：一个感叹号，外加屏幕四周的闪烁边框
static void drawNeedsIcon() {
  tft.fillRect(ICON_CX - 3, ICON_CY - 14, 6, 20, C_NEEDS);
  tft.fillCircle(ICON_CX, ICON_CY + 13, 3, C_NEEDS);
}

static void drawNeedsBorder(bool on) {
  uint16_t color = on ? C_NEEDS : C_BG;
  tft.fillRect(0, 0, SCREEN_W, 2, color);
  tft.fillRect(0, SCREEN_H - 2, SCREEN_W, 2, color);
  tft.fillRect(0, 0, 2, SCREEN_H, color);
  tft.fillRect(SCREEN_W - 2, 0, 2, SCREEN_H, color);
  pumpSerial();
}

// 失联：整屏变橙，中间一个大大的 LOST
static void drawLostScreen() {
  fillScreenSliced(C_LOST);
  tft.setTextSize(4);                     // 放大四倍 = 每字 24×28 像素
  tft.setTextColor(ST77XX_BLACK);
  int16_t width = (int16_t)(4 * 6 * 4);
  tft.setCursor((SCREEN_W - width) / 2, 44);
  // 分两次打印：四个大字一次画完要 5 毫秒出头，超过「连续阻塞不超过 3 毫秒」
  // 的纪律。拆成两个字母一组，中间收一次串口，最坏就只有 2.5 毫秒。
  tft.print(F("LO"));
  pumpSerial();
  tft.print(F("ST"));
  pumpSerial();
}

// ═══════════════════════════ 七、整屏渲染与状态切换 ═══════════════════════════

// 标题用的是**内置字体放大两倍**：正文那种 12 像素的字库适合小字，
// 标题要醒目得多。标题全是英文，内置字体够用。
//
// 注意文字要用 F("...") 包起来 —— 那样它会留在 Flash 里；不包的话会被复制
// 进内存，而内存只有 2KB。长度只能自己数（内置字体每个字符固定 6 像素宽，
// 放大两倍就是 12），所以这里要求调用方一起把长度传进来。
static void printTitle(const __FlashStringHelper *text, uint8_t length, uint16_t color) {
  tft.setTextSize(2);
  tft.setTextColor(color, C_BG);
  tft.setCursor((SCREEN_W - (int16_t)(length * 12)) / 2, TITLE_Y);
  tft.print(text);
  pumpSerial();
}

static uint16_t stateColor(PetState state) {
  switch (state) {
    case ST_WORKING:   return C_WORKING;
    case ST_NEEDS_YOU: return C_NEEDS;
    case ST_DONE:      return C_DONE;
    case ST_ERROR:     return C_ERROR;
    case ST_LOST:      return C_LOST;
    case ST_IDLE:
    default:           return C_IDLE;
  }
}

// 把当前状态整屏画出来。只在**状态真的变了**的时候调用（动画不算）。
static void renderState() {
  if (currentState == ST_LOST) {
    drawLostScreen();
    return;
  }

  fillScreenSliced(C_BG);

  // 标题
  uint16_t color = stateColor(currentState);
  switch (currentState) {
    case ST_WORKING:   printTitle(F("WORKING"), 7, color); break;
    case ST_NEEDS_YOU: printTitle(F("APPROVE?"), 8, color); break;
    case ST_DONE:      printTitle(F("DONE"), 4, color); break;
    case ST_ERROR:     printTitle(F("ERROR"), 5, color); break;
    case ST_IDLE:
    default:           printTitle(F("IDLE"), 4, color); break;
  }

  // 图标
  switch (currentState) {
    case ST_IDLE:      drawIdleIcon(); break;
    case ST_WORKING:   drawWorkingIcon(); break;
    case ST_NEEDS_YOU: drawNeedsIcon(); drawNeedsBorder(true); borderOn = true; break;
    case ST_DONE:      drawDoneIcon(); break;
    case ST_ERROR:     drawErrorIcon(); break;
    default: break;
  }

  // 正文（命令摘要 / 错误信息 / 状态小字）
  if (msgBuf[0] != '\0') drawBodyText(msgBuf);
}

// 状态切换的唯一入口。所有地方都从这里改状态，免得漏掉重绘或记账。
static void setState(PetState state) {
  if (state != ST_LOST) lastValidState = state;
  currentState = state;
  animPhase = 0;
  renderState();
}

// 切换状态并把文案换成 Flash 里的中文常量（省内存的写法）
static void setStateFlash(PetState state, const char *flashText) {
  strcpy_P(msgBuf, flashText);
  setState(state);
}

// 动画：只重画动的那一小块，不整屏刷新（整屏太慢，会丢串口数据）
static void updateAnimation() {
  unsigned long now = millis();
  switch (currentState) {
    case ST_IDLE:
      if (now - lastAnimMs >= ANIM_FRAME_MS) {
        lastAnimMs = now;
        animPhase = (uint8_t)((animPhase + 1) & 0x0F);
        drawIdleIcon();
      }
      break;
    case ST_WORKING:
      if (now - lastAnimMs >= ANIM_FRAME_MS) {
        lastAnimMs = now;
        animPhase++;
        drawWorkingIcon();
      }
      break;
    case ST_NEEDS_YOU:
      if (now - lastBlinkMs >= BLINK_MS) {
        lastBlinkMs = now;
        borderOn = !borderOn;
        drawNeedsBorder(borderOn);
      }
      break;
    default:
      break;
  }
}

// ═══════════════════════════ 八、协议：解析一条消息 ═══════════════════════════

// 把设备要说的话发回电脑（一条消息一行）
static void sendLine(const char *text) {
  Serial.print(text);
  Serial.print('\n');
}

// 回传按钮决策。request_id 原样带回 —— 电脑端靠它确认「这是当前那次审批的按钮」。
static void sendButton(const char *action) {
  Serial.print(F("{\"type\":\"button\",\"request_id\":\""));
  Serial.print(ridBuf);
  Serial.print(F("\",\"action\":\""));
  Serial.print(action);
  Serial.print(F("\"}"));
  Serial.print('\n');
}

// 开机问候：告诉电脑「我刚启动」。电脑端收到会补发当前画面 ——
// 因为**打开串口会让板子复位**，没有这条消息，屏幕会一直停在开机画面上。
static void sendHello() {
  Serial.print(F("{\"type\":\"hello\",\"fw\":\"uno/1.0\"}"));
  Serial.print('\n');
}

// 处理一条完整的行（主循环里调用，不在中断里）
//
// ⚠️ **铁律：先把要用的字段全部取进局部变量，最后才调用会重绘屏幕的函数。**
//
// 原因：画屏幕的代码里会穿插 pumpSerial()，也就是「一边画一边继续收串口」。
// 收进来的新字节就写在**当前这一行所在的缓冲**里（单缓冲，省内存）——
// 所以 setState() 之后再去读 line，读到的可能就是下一条消息的字节了。
// 现在的写法是每条分支都在最后一句才 setState()，解析早就结束了。
// 将来往这些分支里加代码时，请守住这条规矩。
static void dispatchLine() {
  char *line = rxLine.buf;

  // 先看这是什么类型的消息。取不到 type 就说明不是我们的报文，直接忽略。
  char typeBuf[18];
  VpSink type;
  vpSinkInit(&type, typeBuf, sizeof(typeBuf));
  if (!vpJsonScan(line, "type", &type)) {
    dbg(F("收到无法识别的行，已忽略"));
    return;
  }

  // 走到这里说明是一条合法的消息 —— 刷新保活计时器。
  // 注意：**任何**消息都算保活，不只是心跳，这样只要电脑还在说话，屏幕就不会
  // 误报失联。
  lastHeartbeat = millis();

  if (strcmp(typeBuf, "heartbeat") == 0) return;   // 心跳只为保活，不改画面

  if (strcmp(typeBuf, "state") == 0) {
    char statusBuf[12];
    VpSink status;
    vpSinkInit(&status, statusBuf, sizeof(statusBuf));
    if (!vpJsonScan(line, "status", &status)) return;

    // 取正文（没有 msg 字段就清空）
    VpSink msg;
    vpSinkInit(&msg, msgBuf, sizeof(msgBuf));
    vpJsonScan(line, "msg", &msg);

    // 收到新状态就作废待审批的请求：这条状态说明上一次审批已经翻篇了，
    // 此时再按按钮不该有任何效果（防止「幽灵批准」）。
    ridBuf[0] = '\0';

    if (strcmp(statusBuf, "idle") == 0)         setState(ST_IDLE);
    else if (strcmp(statusBuf, "working") == 0) setState(ST_WORKING);
    else if (strcmp(statusBuf, "done") == 0)    setState(ST_DONE);
    else if (strcmp(statusBuf, "error") == 0)   setState(ST_ERROR);
    else dbg(F("未知的状态值，已忽略"));
    return;
  }

  if (strcmp(typeBuf, "approval_request") == 0) {
    // request_id：原样存下来，按钮回传时要带上
    VpSink rid;
    vpSinkInit(&rid, ridBuf, sizeof(ridBuf));
    if (!vpJsonScan(line, "request_id", &rid)) {
      ridBuf[0] = '\0';
      return;
    }

    // 先把工具名取出来，再取摘要，最后拼成「工具名: 命令」。
    // 顺序不能反：摘要要用的内存 = 总上限 - 工具名那一段。
    char toolBuf[26];
    VpSink tool;
    vpSinkInit(&tool, toolBuf, sizeof(toolBuf));
    bool hasTool = vpJsonScan(line, "tool", &tool);

    uint16_t reserve = hasTool ? (uint16_t)(strlen(toolBuf) + 2) : 0;
    VpSink summary;
    vpSinkInit(&summary, msgBuf, (uint16_t)(sizeof(msgBuf) - reserve));
    if (!vpJsonScan(line, "summary", &summary)) msgBuf[0] = '\0';

    if (hasTool && toolBuf[0] != '\0') {
      // 把已经写好的摘要整体后移，前面腾出「工具名: 」的位置
      uint16_t summaryLen = (uint16_t)strlen(msgBuf);
      uint16_t toolLen = (uint16_t)strlen(toolBuf);
      memmove(msgBuf + toolLen + 2, msgBuf, (size_t)summaryLen + 1);
      memcpy(msgBuf, toolBuf, toolLen);
      msgBuf[toolLen] = ':';
      msgBuf[toolLen + 1] = ' ';
    }

    setState(ST_NEEDS_YOU);
    beepStart(1, 120, 0);   // 提示音：响一声，告诉用户「该你了」
    return;
  }

  dbg(F("未知的消息类型，已忽略"));
}

// ═══════════════════════════ 九、按钮 ═══════════════════════════
//
// 按钮接法：一端接引脚、一端接 GND，引脚启用内部上拉。所以
// **不按 = 高电平，按下 = 低电平**。
//
// 去抖：机械按钮在按下的瞬间会「抖」出好几个通断，所以按下后 200 毫秒内的
// 重复信号统统不算。

static void scanButtons() {
  unsigned long now = millis();

  if (digitalRead(PIN_BTN_APPROVE) == LOW && (now - lastApproveMs) > DEBOUNCE_MS) {
    lastApproveMs = now;
    if (ridBuf[0] != '\0') {
      sendButton("approve");
      ridBuf[0] = '\0';                       // 作废：一次审批只认一次按键
      setStateFlash(ST_WORKING, STR_APPROVED);  // 本地立刻切画面，不等电脑回话
      beepStart(2, 35, 30);
    }
  }

  if (digitalRead(PIN_BTN_DENY) == LOW && (now - lastDenyMs) > DEBOUNCE_MS) {
    lastDenyMs = now;
    if (ridBuf[0] != '\0') {
      sendButton("deny");
      ridBuf[0] = '\0';
      setStateFlash(ST_IDLE, STR_DENIED);
      beepStart(1, 120, 0);
    }
  }
}

// ═══════════════════════════ 十、蜂鸣器（非阻塞）═══════════════════════════
//
// 这一段是整个程序里最容易被写错的地方，所以单独说明。
//
// 「响两声」听起来简单，但如果写成：
//     tone(4, 2700); delay(35); noTone(4); delay(30); ...
// 那么在响的这 100 多毫秒里，主循环完全停住 —— 串口来的字节没人接，
// 缓冲一满就丢。丢掉的可能是电脑刚发来的整条审批请求。
//
// 所以这里把它拆成「设定节奏」和「按节奏推进」两半：beepStart() 只说
// 「响两声、每声 35 毫秒」，真正的开关动作交给 beepService() 每轮循环推进一点点。

static uint8_t beepTimes = 0;        // 还剩几声没响
static bool beepOn = false;          // 现在是不是正在响
static uint16_t beepOnMs = 0;
static uint16_t beepOffMs = 0;
static unsigned long beepStamp = 0;

static void beepStart(uint8_t times, uint16_t on_ms, uint16_t off_ms) {
  if (times == 0) return;
  beepTimes = times;
  beepOnMs = on_ms;
  beepOffMs = off_ms;
  beepStamp = millis();
  beepOn = true;
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, HIGH);
#else
  tone(PIN_BUZZER, BUZZER_TONE_HZ);   // 无源蜂鸣器：必须给方波才响
#endif
}

static void beepService() {
  if (beepTimes == 0) return;
  unsigned long now = millis();

  if (beepOn) {
    if (now - beepStamp >= beepOnMs) {
#if BUZZER_ACTIVE
      digitalWrite(PIN_BUZZER, LOW);
#else
      noTone(PIN_BUZZER);
#endif
      beepOn = false;
      beepStamp = now;
    }
    return;
  }

  if (now - beepStamp >= beepOffMs) {
    beepTimes--;
    if (beepTimes > 0) {
      beepStamp = now;
      beepOn = true;
#if BUZZER_ACTIVE
      digitalWrite(PIN_BUZZER, HIGH);
#else
      tone(PIN_BUZZER, BUZZER_TONE_HZ);
#endif
    }
  }
}

// ═══════════════════════════ 十一、看门狗 ═══════════════════════════
//
// 电脑端每秒发一次心跳。如果 5 秒都没收到**任何**消息，就说明链路断了
// （线掉了、daemon 关了、电脑睡了），屏幕立刻切成橙色的「失联」——
// 绝不能让它停在最后一个状态上骗你说「AI 还在干活」。

static void checkWatchdog() {
  bool isLost = (millis() - lastHeartbeat) > WATCHDOG_TIMEOUT;

  if (isLost && currentState != ST_LOST) {
    // 进入失联。**不走 setState** —— 失联画面不显示正文，而且我们要保住
    // lastValidState / msgBuf，等链路恢复了再拿回来用。
    ridBuf[0] = '\0';       // 审批请求作废：这期间按按钮也送不出去
    currentState = ST_LOST;
    renderState();
  } else if (!isLost && currentState == ST_LOST) {
    // 恢复。回到失联前的状态；但如果那是一条审批卡，就退回空闲 ——
    // 那张卡可能早就过期了，显示出来只会骗你去按一个没用的按钮。
    PetState back = (lastValidState == ST_NEEDS_YOU) ? ST_IDLE : lastValidState;
    setState(back);
  }

  // 指示灯：链路正常才亮
  digitalWrite(PIN_LED_STATUS, isLost ? LOW : HIGH);
}

// ═══════════════════════════ 十二、启动与主循环 ═══════════════════════════

void setup() {
  Serial.begin(115200);

  pinMode(PIN_BTN_APPROVE, INPUT_PULLUP);
  pinMode(PIN_BTN_DENY, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_STATUS, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);   // 无源蜂鸣器空闲保持低电平，否则会一直响

  tft.initR(INITR_BLACKTAB);       // 黑排 1.77" 模块；花屏就换 INITR_GREENTAB / INITR_REDTAB
  tft.setRotation(1);              // 横屏：160 宽 × 128 高
  tft.fillScreen(C_BG);

  vpLineInit(&rxLine);
  ridBuf[0] = '\0';

  // 开机画面：先打个招呼，让用户知道板子活着
  strcpy_P(msgBuf, STR_READY);
  setState(ST_IDLE);
  sendHello();

  lastHeartbeat = millis();   // 从现在开始算保活，别一上电就报失联
}

void loop() {
  // ① 收：把串口字节搬进行缓冲（很快，几微秒）
  pumpSerial();

  // ② 看：攒够一整条就处理掉。
  //    处理放在这里而不是「收到就处理」，是为了避免在画屏幕的中途又触发一次
  //    画屏幕（那样会递归，2KB 的栈扛不住）。
  //
  //    为什么是 while 而不是 if：处理一条消息可能要重绘整屏几十毫秒，这期间
  //    串口会继续收字节 —— 完全可能又攒够了一整条（比如一条心跳）。
  //    先摘旗再处理，新攒够的那条会把旗重新立起来，于是下一轮接着处理。
  while (rxLine.ready) {
    vpLineConsume(&rxLine);
    dispatchLine();
  }

  // ③ 显示与交互
  checkWatchdog();
  scanButtons();
  beepService();
  updateAnimation();

  delay(2);   // 让出一点时间，串口芯片和屏幕都喘口气
}
