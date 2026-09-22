/*
 * VibePet 设备端固件
 * AI 编程助手物理状态显示与审批终端 —— ESP32-C3 SuperMini + 1.77" ST7735S TFT
 *
 * ═══════════════ 新手导读：这个程序是怎么跑起来的 ═══════════════
 *
 * 如果你以前没写过单片机，先记住一件事：**这里没有「程序跑完就结束」这回事**。
 * 单片机的程序只有两个入口：
 *
 *     setup()  —— 开机时跑一次，用来做准备工作（配引脚、点亮屏幕、开蓝牙）
 *     loop()   —— 之后**无限循环**，一秒转几十圈，永远不停
 *
 * 所以本文件的结构就是：所有准备工作塞进 setup()，所有「需要一直盯着的事」
 * 塞进 loop() —— 比如看有没有新消息、看按钮被按了没、看该不该重画动画。
 *
 * ⚠️ 最要紧的一个概念：**loop() 里绝对不能长时间卡住**。
 * 它一卡，按钮就没人扫了、动画就停了、心跳也忘了看。所以下面凡是需要
 * 「等一会儿」的地方（比如蜂鸣器响 60 毫秒），都是算好了才敢停的。
 *
 * ═══════════ 两个「世界」，以及它们为什么不能随便说话 ═══════════
 *
 * 这颗芯片上同时跑着两拨代码：
 *
 *   主循环（loop）    —— 你写的代码：画面、按钮、各种判断
 *   BLE 任务（回调）  —— 系统在后台跑的：蓝牙一收到数据，它立刻执行
 *
 * 关键点：**它们是并发的**。你正在 loop 里画屏幕时，BLE 回调可能突然插进来。
 * 要是两边同时去改同一个字符串，程序就可能崩掉。
 *
 * 本程序的应对办法是「投递」而不是「共享」：
 *   蓝牙收到数据 → 回调只负责按换行切开 → 扔进队列 → 主循环从队列里取出来
 *   慢慢解析。两边只碰那个队列，而队列本身是线程安全的。
 *
 * ═══════════════ 想跳着读的话 ═══════════════
 *
 *   ① 先看「配置」区 —— 引脚、蜂鸣器类型、各种时间常量都在那儿；
 *   ② 然后跳到文件最底下的 setup() 和 loop() —— 那是程序的主干；
 *   ③ 再回来看各个功能块（画面 / 按钮 / 看门狗 / 蓝牙）。
 *
 * 职责：
 *   1. 通过 BLE NUS 接收电脑端 bridge_daemon.py 发来的 JSON Lines
 *   2. 在屏幕上显示六种状态（idle / working / needs_you / done / error / 失联）
 *   3. 两个物理按钮完成 approve / deny，按 request_id 原样回传
 *
 * 协议见设计文档 3.3，模块划分对应 5.2。
 *
 * ────────────────── 烧录前必须确认的两件事 ──────────────────
 *
 *   1. TFT_eSPI 库的 User_Setup.h 要按 firmware/TFT_eSPI_User_Setup.h 配置。
 *      驱动宏与初始化序列选错会花屏、偏色或显示区域偏移，须逐个试。
 *
 *   2. 下面 BUZZER_ACTIVE 要与你实际焊上去的蜂鸣器一致（本项目选型是无源）。
 *
 * ────────────────── 与设计文档 5.5 示例代码的差异 ──────────────────
 *
 *   · 画布：示例按 0.96" 的 80×160 写，setRotation(1) 得到 160×80；
 *     本固件面向 1.77" 的 128×160，横屏是 160×128，所有坐标已重算。
 *   · 动画：示例每帧 fillScreen 全屏重绘，屏幕上会明显闪；这里改成只重绘
 *     图标所在的矩形区域。
 *   · BLE 回调：示例在回调里直接解析 JSON 并改全局状态，而回调运行在 BLE
 *     任务里，与 loop() 并发操作 String 有崩溃风险。这里回调只负责按 \n
 *     切分并投递到 FreeRTOS 队列，解析全部放到主循环。
 *   · 文字：标题类大字仍用内置 GLCD 字体（ASCII），正文（命令摘要 / 错误
 *     信息 / 状态副标题）改用 U8g2_for_TFT_eSPI 的 wqy12 GB2312 中文字体，
 *     中文可以正常显示（见 FONT_BODY）。
 */

#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <math.h>

// ════════════════════════════ 配置 ════════════════════════════
//
// 这一区集中放「想调就改这儿」的东西：外设接在哪个脚、蜂鸣器是哪种、
// 各种时间常量。下面的代码全都依赖这些值。

// —— 引脚（设计文档 4.2 / 4.3，已避开 GPIO9 启动引脚）——
//
// 每个外设接在芯片的哪个脚上；完整的接线表和接线图见设计文档 4.7。
// 注意 **GPIO9 不能用**：它是芯片的「启动模式脚」，接东西上去会导致设备
// 一开机就进下载模式、或者干脆不启动 —— 所以本方案全程避开它。
#define PIN_BTN_APPROVE  1    // 批准按钮
#define PIN_BTN_DENY     10   // 拒绝按钮
#define PIN_BUZZER       3    // 蜂鸣器
#define PIN_LED_STATUS   2    // 连上蓝牙就亮的那颗指示灯

// 蜂鸣器类型 —— 本项目选型是无源（设计文档 4.3）：
//   0 = 无源蜂鸣器：内部没有振荡源，必须靠 PWM 方波驱动才出声。给恒定
//       直流电平只会听到一声「咔哒」，随后无声。
//   1 = 有源蜂鸣器模块（「3 针低电平触发」那类）：内部自带振荡源，给恒定
//       电平就持续发声，因此空闲必须保持高电平，否则上电即长鸣。
#define BUZZER_ACTIVE 0

// 无源蜂鸣器的驱动频率（Hz），2700 接近常见无源蜂鸣器的谐振点，最响。
#define BUZZER_TONE_HZ 2700

// —— BLE 协议（设计文档 3.3）——
//
// 这几个是「和电脑端说好的暗号」，必须和 pc/bridge_daemon.py 里一字不差。
// 蓝牙低功耗里，每个服务用一串 UUID 当门牌号；NUS（Nordic UART Service）
// 是「把蓝牙当串口用」的通用约定，全世界都认这个门牌号，不是我们编的。
#define DEVICE_NAME      "VibePet"                                // 广播时用的名字
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // 电脑 → 设备
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // 设备 → 电脑

// —— 时序（设计文档 2.2 / 5.4）——
//
// 四个「等多久」的约定。单位都是毫秒（1000 毫秒 = 1 秒）。
// 它们不是随便定的，每一条背后都有理由。

// 看门狗超时：超过这么久没收到电脑端的**任何**消息，就认定「失联」，
// 屏幕切成橙色 LOST。电脑端每秒发一次心跳，所以 5 秒 ≈ 连续丢 4 个包。
const unsigned long WATCHDOG_TIMEOUT = 5000;

// 按钮去抖时间。机械按钮按下去的瞬间，触点会「抖」出好几个通-断-通，
// 不处理的话按一次会被当成按了好几次。这里的方法是：认下第一次之后，
// 200 毫秒内不再搭理它。
const unsigned long DEBOUNCE_MS      = 200;

// 动画帧间隔，约每秒 20 帧。
// 为什么不做 60 帧？屏幕走 SPI 刷不了那么快，而且肉眼也看不出区别。
const unsigned long ANIM_INTERVAL    = 50;

// APPROVE? 边框闪烁的半周期（亮 500 毫秒、暗 500 毫秒，来回闪）。
const unsigned long BLINK_INTERVAL   = 500;

// —— 画布 ——
// 1.77" ST7735S 原生 128×160，setRotation(1) 后横屏为 160×128。
// （屏幕被我们「横过来」用了，所以宽度 160、高度 128。）
#define SCREEN_W 160
#define SCREEN_H 128

// 动画只重绘这块区域，其余部分保持不动，避免整屏闪烁
// ICON_CX / ICON_CY 是图标中心点的坐标，ICON_R 是它占的半径。
#define ICON_CX 80
#define ICON_CY 36
#define ICON_R  22

// —— 消息队列 ——
//
// 这就是导读里说的那条「投递通道」：BLE 回调往里放，主循环从里取。
//
// BLE 回调运行在 BLE 任务，解析在主循环，两者不能直接共享 String。
// 512 字节：电脑端单字段（summary/msg）上限 240 字节，带中文的
// approval_request 整行约 330 字节，留足余量；队列 8×512 = 4 KB。
#define LINE_MAX         512   // 一行消息最长多少字节（超了整条丢弃）
#define LINE_QUEUE_DEPTH 8     // 队列里最多排几条（排满就丢新的）

// ════════════════════════════ 全局状态 ════════════════════════════
//
// 「全局变量」就是定义在所有函数外面的变量，任何函数都能读能改。
// 单片机程序里这是常态 —— 因为 loop() 要反复跑，总得有个地方记住
// 「现在是什么情况」。下面这些变量，就是这个程序的「记忆」。

// 屏幕能显示的六种状态。enum 是「枚举」，作用是给几个数字起名字：
// 写 ST_IDLE 比写 0 好懂得多，而且写错了编译器会报错。
enum PetState { ST_IDLE, ST_WORKING, ST_NEEDS_YOU, ST_DONE, ST_ERROR, ST_LOST };

// ── 三个跟硬件打交道的对象 ──
TFT_eSPI tft = TFT_eSPI();    // 屏幕：tft.fillScreen() / tft.print() 都靠它
U8g2_for_TFT_eSPI u8f;        // 正文的中文字体渲染（TFT_eSPI 内置字体只有 ASCII）
NimBLECharacteristic* txChar = nullptr;  // 蓝牙「发送通道」，setup 时才真正创建
QueueHandle_t lineQueue = nullptr;       // 导读里那条消息队列，同样 setup 才创建

// 蓝牙连上了吗？标 volatile 是在告诉编译器「这个变量可能被别的任务改掉，
// 别自作主张把它缓存到寄存器里」—— 它确实会被 BLE 回调改，而主循环在读。
volatile bool bleConnected = false;

// ── 现在屏幕上是什么 ──
PetState currentState = ST_IDLE;
String statusMsg      = "";   // error 的错误信息 / needs_you 的命令摘要
String currentRequestId = ""; // 仅在有审批请求时非空

// 失联恢复用：最近一个非 LOST 状态及其文案。看门狗把画面改成 ST_LOST 前
// 先记在这里，心跳恢复时切回去（而不是像以前那样重画一遍 LOST）。
PetState lastValidState = ST_IDLE;
String lastValidMsg = "";

// ── 一串「上次是什么时候」的时间戳 ──
// 思路都一样：先记住上次做这件事的时刻，下次想做之前先算算隔了多久。
// （millis() 返回「开机到现在过了多少毫秒」，下面到处都在用它。）
unsigned long lastHeartbeat = 0;   // 上次收到电脑端消息的时刻（看门狗靠它判断失联）
bool lostShown = false;            // 画面是不是已经切成 LOST 了

unsigned long lastApproveMs = 0;   // 上次「批准」按钮被认下的时刻（去抖用）
unsigned long lastDenyMs    = 0;   // 上次「拒绝」按钮被认下的时刻

unsigned long lastAnimMs  = 0;     // 上次画动画帧的时刻
unsigned long lastBlinkMs = 0;     // 上次切换闪烁边框的时刻
bool blinkOn = true;               // 闪烁边框这一拍是亮还是暗
float animPhase = 0.0f;            // 动画相位：每帧加一点，用来算圆点大小 / 方块角度

// 颜色别名（六种状态的主色，见设计文档 3.3）
// 直接写 TFT_GREEN 也行，但起个 C_WORK 这样的名字，读起来更容易和
// 「working 用绿色」这条规则对上号。
#define C_BG    TFT_BLACK
#define C_IDLE  TFT_WHITE
#define C_WORK  TFT_GREEN
#define C_NEED  TFT_YELLOW
#define C_DONE  TFT_CYAN
#define C_ERR   TFT_RED
#define C_LOST  TFT_ORANGE

// ════════════════════════════ 显示辅助 ════════════════════════════
//
// 这一区是「画图工具箱」：把常用的画字、画线、折行包成函数，
// 下面画六种状态时会反复调用它们。

// 把一行字画在屏幕水平居中的位置。
// y 是垂直坐标；(SCREEN_W - w) / 2 就是「(屏宽 − 字宽) ÷ 2」，即左右留白相等。
void drawCentered(const char* text, int y, uint8_t size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  int w = tft.textWidth(text);
  int x = (SCREEN_W - w) / 2;
  if (x < 0) x = 0;   // 字太长顶到边了，别算出负数坐标
  tft.setCursor(x, y);
  tft.print(text);
}

// 粗线：沿垂直方向偏移画多条，否则 1px 的对勾/叉号在小屏上太细
void thickLine(int x0, int y0, int x1, int y1, uint16_t color, int w = 3) {
  // 先算这条线的方向，以及垂直于它的方向 (nx, ny)
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;      // 起终点重合，没线可画
  float nx = -dy / len, ny = dx / len;
  // 再沿垂直方向并排画好几条一样长的线 —— 看上去就是一条粗线
  int half = w / 2;
  for (int i = -half; i <= half; i++) {
    tft.drawLine((int)(x0 + nx * i), (int)(y0 + ny * i),
                 (int)(x1 + nx * i), (int)(y1 + ny * i), color);
  }
}

// ── 正文（摘要 / 错误信息 / 状态副标题）用 u8g2 的中文字体渲染 ──
// wqy12 = 文泉驿点阵宋体 12px，覆盖完整 GB2312（约 200 KB Flash，仅此一处
// 字体数据）。标题类大字仍是 GLCD，两种字体混排视觉上接近。
#define FONT_BODY u8g2_font_wqy12_t_gb2312

/*
 * 控制字符（\n \t 等）会让折行与光标错位，替换成空格；其余字节原样保留。
 * 不再像旧版 sanitizeAscii 那样把中文替换成 '?' —— 中文交给 FONT_BODY 渲染。
 */
String cleanControl(const String& src) {
  String out;
  out.reserve(src.length());   // 先要够地方，免得边加边扩容
  for (size_t i = 0; i < src.length(); i++) {
    uint8_t c = (uint8_t)src[i];
    // 比 0x20 小的都是控制字符（换行、制表符……），0x7F 是删除符。
    // 汉字的字节都大于 0x80，所以不会被误伤。
    out += (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
  }
  return out;
}

// 返回 s[i] 起始的 UTF-8 码点字节数（1~4）；非法前缀字节按 1 处理
//
// 为什么需要它？因为一个汉字在 UTF-8 里占 3 个字节，不能像英文那样
// 「一个字符 = 一个字节」地往后挪 —— 挪错位置就切出乱码了。
// 这个函数看第一个字节的开头几位，就能判断出「这个字总共占几个字节」。
int utf8Step(const String& s, int i) {
  uint8_t c = (uint8_t)s[i];
  if (c < 0x80) return 1;              // 0xxxxxxx —— 普通 ASCII 字符
  if ((c & 0xE0) == 0xC0) return 2;    // 110xxxxx —— 两字节字符的开始
  if ((c & 0xF0) == 0xE0) return 3;    // 1110xxxx —— 三字节（汉字在这里）
  if ((c & 0xF8) == 0xF0) return 4;    // 11110xxx —— 四字节（emoji 之类）
  return 1;                            // 残缺字节，按 1 走，免得卡死
}

// 按屏宽折行输出，最多 maxLines 行；放不下时末行以 ".." 收尾
//
// 这是全文件最绕的一段，但目标很朴素：把一段可能很长的中文，按屏幕宽度
// 切成几行显示出来；行数不够时，在末尾画个「..」表示后面还有。
void drawWrapped(const String& raw, int y, uint16_t color, int maxLines) {
  const String text = cleanControl(raw);
  u8f.setFont(FONT_BODY);
  u8f.setForegroundColor(color);

  const int maxWidth = SCREEN_W - 8;   // 左右各留 4 像素边距
  const int lineHeight = u8f.getFontAscent() - u8f.getFontDescent() + 2;
  const int len = (int)text.length();

  // 从 start 起贪心取最长的、渲染宽度不超过 limit 的 UTF-8 前缀长度。
  // 文本很短（≤240 字节），逐字符测宽的平方复杂度只在状态切换时跑一次。
  //
  // 所谓「贪心」就是：一个字一个字往这行里加，加到「再加一个就超宽」为止。
  // 注意每次加的是「一整个字」而不是一个字节，靠的就是上面那个 utf8Step。
  auto greedyTake = [&](int start, int limit) -> int {
    int take = 0;
    while (start + take < len) {
      int step = utf8Step(text, start + take);
      if (start + take + step > len) step = len - start - take;  // 尾部残缺字节
      const int w = u8f.getUTF8Width(text.substring(start, start + take + step).c_str());
      if (w > limit) {
        if (take == 0) return step;   // 单字符就超宽：至少输出它，防死循环
        break;
      }
      take += step;
    }
    return take;
  };

  // 一行一行地切、画。末行要特殊照顾：如果切完还有剩，就得给结尾那个
  // ".." 腾地方，所以最后一行要按「减掉 .. 的宽度」重新切一次。
  int pos = 0, line = 0;
  while (pos < len && line < maxLines) {
    int take = greedyTake(pos, maxWidth);
    // 末行放不下剩余内容时，给 ".." 让位重新折一次
    if (line == maxLines - 1 && pos + take < len) {
      take = greedyTake(pos, maxWidth - u8f.getUTF8Width(".."));
      if (take == 0) break;
    }
    const int baseline = y + line * lineHeight + u8f.getFontAscent();
    const String chunk = text.substring(pos, pos + take);
    u8f.drawUTF8(4, baseline, chunk.c_str());   // 左边固定从 4 像素开始
    if (line == maxLines - 1 && pos + take < len) {
      u8f.drawUTF8(4 + u8f.getUTF8Width(chunk.c_str()), baseline, "..");
    }
    pos += take;
    line++;
  }
}

// ════════════════════════════ 六种状态绘制 ════════════════════════════
//
// 屏幕上的画面分两份：
//   · 图标 —— 会动的那个圆点 / 方块 / 对勾，画在上方固定的一小块区域里；
//   · 文字 —— 大字标题 + 下面几行说明，画在下方。
//
// 为什么要分开？因为图标要一直动（每秒 20 帧），文字却是静止的。
// 要是每帧把整屏重画一遍，屏幕会明显闪 —— 所以下面这些函数都只重画
// 需要变的那一小块。

// 把图标那一小块涂成背景色，相当于「橡皮擦」。
// 每一帧都要先擦干净再画新的，否则新旧图像会叠在一起、糊成一团。
void clearIconArea() {
  tft.fillRect(ICON_CX - ICON_R, ICON_CY - ICON_R, ICON_R * 2, ICON_R * 2, C_BG);
}

// IDLE 的图标：一个会「呼吸」的白色圆点。
// 大小用 sin 算 —— sin 的值在 −1 到 1 之间来回摆，所以半径就在 8 到 18 之间来回变。
void drawIdleIcon() {
  clearIconArea();
  int r = 8 + (int)(5.0f * (1.0f + sinf(animPhase)));  // 半径在 8~18 之间呼吸
  tft.fillCircle(ICON_CX, ICON_CY, r, C_IDLE);
}

// WORKING 的图标：一个转动的绿色方块。
// 做法：在圆周上均匀取 4 个点（彼此相差 90°），再用两个三角形拼成方块。
// animPhase 每帧加一点，这 4 个点跟着转，「方块」看起来就在旋转。
void drawWorkingIcon() {
  clearIconArea();
  const int half = 13;                  // 方块半径：中心到顶点的距离
  float pts[4][2];                      // 4 个顶点，每个记 x 和 y
  for (int i = 0; i < 4; i++) {
    float a = animPhase + i * (PI / 2.0f);   // 每个点相隔 90°
    pts[i][0] = ICON_CX + half * cosf(a);
    pts[i][1] = ICON_CY + half * sinf(a);
  }
  // 画两个三角形（顶点 0-1-2 和 0-2-3），拼起来正好是那个方块
  tft.fillTriangle((int)pts[0][0], (int)pts[0][1], (int)pts[1][0], (int)pts[1][1],
                   (int)pts[2][0], (int)pts[2][1], C_WORK);
  tft.fillTriangle((int)pts[0][0], (int)pts[0][1], (int)pts[2][0], (int)pts[2][1],
                   (int)pts[3][0], (int)pts[3][1], C_WORK);
}

// DONE 的图标：一个对勾。就是两条粗线接在一起（先短促向下，再长长地扬上去）。
void drawDoneIcon() {
  clearIconArea();
  thickLine(ICON_CX - 16, ICON_CY + 1, ICON_CX - 4, ICON_CY + 13, C_DONE);
  thickLine(ICON_CX - 4, ICON_CY + 13, ICON_CX + 17, ICON_CY - 11, C_DONE);
}

// ERROR 的图标：一个叉。两条对角线交叉。
void drawErrorIcon() {
  clearIconArea();
  thickLine(ICON_CX - 13, ICON_CY - 13, ICON_CX + 13, ICON_CY + 13, C_ERR);
  thickLine(ICON_CX + 13, ICON_CY - 13, ICON_CX - 13, ICON_CY + 13, C_ERR);
}

// 各状态的整屏渲染。状态切换时调用一次，之后由动画帧局部更新。
//
// 这是「一次性把整屏画好」的地方 —— 状态一变就调它。之后除非状态再变、
// 或收到新的文字，屏幕上就只有图标那一小块在动。
void renderState() {
  tft.fillScreen(C_BG);   // 先整屏刷成背景色，把上一个状态擦干净

  // switch 的意思是「看 currentState 是哪一个，就跳去做对应那段」。
  switch (currentState) {
    case ST_IDLE:
      drawIdleIcon();
      drawCentered("IDLE", 70, 2, C_IDLE);      // 70 是纵坐标，2 是字号倍数
      if (statusMsg.length()) drawWrapped(statusMsg, 92, TFT_WHITE, 2);
      break;

    case ST_WORKING:
      drawWorkingIcon();
      drawCentered("WORKING", 70, 2, C_WORK);
      if (statusMsg.length()) drawWrapped(statusMsg, 92, TFT_WHITE, 2);
      break;

    case ST_NEEDS_YOU:
      // 审批卡没有大图标：重点是把命令摘要尽量多地显示出来
      drawCentered("APPROVE?", 8, 2, C_NEED);
      // 4 行 × 约 15px 行高，止于提示行上方；实测行高后可再调
      drawWrapped(statusMsg, 34, TFT_WHITE, 4);
      drawCentered("[v] approve   [x] deny", 112, 1, TFT_DARKGREY);
      break;

    case ST_DONE:
      drawDoneIcon();
      drawCentered("DONE", 70, 2, C_DONE);
      if (statusMsg.length()) drawWrapped(statusMsg, 92, TFT_WHITE, 2);
      break;

    case ST_ERROR:
      drawErrorIcon();
      drawCentered("ERROR", 66, 2, C_ERR);
      drawWrapped(statusMsg, 90, TFT_WHITE, 2);
      break;

    case ST_LOST:
      // 失联最显眼：整屏橙色，一眼就看出不对劲
      tft.fillScreen(C_LOST);
      drawCentered("LOST", 48, 3, TFT_BLACK);
      drawCentered("no heartbeat", 88, 1, TFT_BLACK);
      break;
  }
}

// 动画帧：只更新会动的部分
//
// loop() 每隔一小段时间调它一次。注意每个 case 里只擦、只画图标，
// 绝不碰文字 —— 这就是「画面不闪」的诀窍。
void updateAnimation() {
  switch (currentState) {
    case ST_IDLE:
      animPhase += 0.16f;     // 相位往前走一点，圆点的呼吸就变一点点
      drawIdleIcon();
      break;

    case ST_WORKING:
      animPhase += 0.22f;     // 这个步子大一点，方块转得快些
      drawWorkingIcon();
      break;

    case ST_NEEDS_YOU:
      // 边框闪烁提示「在等你」，只重画四条边，不动文字
      blinkOn = !blinkOn;     // 亮变暗、暗变亮
      // 画两层边框，看起来更醒目
      tft.drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, blinkOn ? C_NEED : C_BG);
      tft.drawRect(2, 2, SCREEN_W - 4, SCREEN_H - 4, blinkOn ? C_NEED : C_BG);
      break;

    default:
      break;  // done / error / lost 是静态画面，没有动画
  }
}

// ════════════════════════════ 蜂鸣器 ════════════════════════════
//
// 只在关键时候叫一声，把人从别的窗口拉回来（比如「有东西等你批准」）。

// 响一声，持续 ms 毫秒。
//
// 注意这是个**阻塞**函数：它用 delay 停在那里，这段时间 loop() 是停住的。
// 之所以敢这么做，是因为一次只停几十毫秒，而且这期间蓝牙消息会堆在队列里、
// 按钮晚几十毫秒扫也来得及。（要是停几秒就不行了。）
void beepOnce(unsigned long ms) {
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, LOW);   // 「低电平触发」模块：给恒定电平即发声
  delay(ms);
  digitalWrite(PIN_BUZZER, HIGH);
#else
  // 无源蜂鸣器：靠方波驱动。noTone() 必须显式调用 —— 它停振并释放
  // LEDC 通道；tone() 的 duration 由 core 的 tone task 异步兜底，
  // 真正的节拍来自这个 delay，两者一起保证「响 ms 毫秒」。
  tone(PIN_BUZZER, BUZZER_TONE_HZ, ms);
  delay(ms);
  noTone(PIN_BUZZER);
#endif
}

// 响 times 声，每声 ms 毫秒，声与声之间歇 70 毫秒。
// 比如 beep(2) 就是「嘀、嘀」两下。
//
// 阻塞式发声。loop 被挡住不影响收包 —— BLE 回调在独立任务里，
// 消息会堆在队列中，这里返回后再消费。
void beep(int times, unsigned long ms = 60) {
  for (int i = 0; i < times; i++) {
    beepOnce(ms);
    if (i < times - 1) delay(70);   // 最后一声后面不用再歇
  }
}

// ════════════════════════════ 状态切换 ════════════════════════════
//
// 所有「换个状态显示」的地方都走这个函数。好处是切换时的杂事
// （记下有效状态、重置动画、重画屏幕）只用写一遍。

void setState(PetState next, const String& msg = "") {
  if (next != ST_LOST) {           // LOST 是看门狗临时覆盖的画面，不算「有效状态」
    lastValidState = next;         // 记下来 —— 失联恢复时要切回到这里
    lastValidMsg = msg;
  }
  currentState = next;
  statusMsg = msg;
  animPhase = 0.0f;                // 动画从头开始，免得新图标从半路转起
  blinkOn = true;                  // 新画面从「亮」这一拍开始闪
  renderState();                   // 立刻重画整屏
}

// ════════════════════════════ 消息处理 ════════════════════════════
//
// 这里是「电脑说的话怎么理解」。主循环从队列里取出一条消息就交给这个函数，
// 由它翻译成人话、决定屏幕该变成什么样。
//
// 电脑端一共会发三种消息：
//   heartbeat        心跳，只是证明「我还活着」，屏幕不用动
//   state            状态变了（idle / working / done / error）
//   approval_request 有东西要你批准 ← 最重要的那条

void processLine(const char* line) {
  // ArduinoJson 7 里 StaticJsonDocument<N> 只是 JsonDocument 的兼容壳：池是动态的
  // （堆分配、按需增长），<512> 既不限制也不预留内存，别把它当成 v6 那种
  // 「栈上定长池」的安全保证。真正的内存上限来自协议 —— 整行 ≤ LINE_MAX(512)
  // 字节，见设计文档 3.3 与 pc/bridge_daemon.py 的 MAX_FIELD_BYTES。
  StaticJsonDocument<512> doc;
  // 把这一行文本解析成「可以按名字取值」的结构。解析失败说明是条坏消息，
  // 丢掉就好 —— 绝不能因为一条坏消息把设备搞崩。
  if (deserializeJson(doc, line) != DeserializationError::Ok) {
    Serial.printf("[vibepet] JSON 解析失败，已忽略: %s\n", line);
    return;
  }

  // 任何一条合法消息都算保活信号（不只是 heartbeat 包）。
  // 设备端是这么设计的：daemon 断线后不再有任何消息，看门狗才超时。
  lastHeartbeat = millis();

  // 取出消息类型。取不出来就说明不认识，直接返回。
  const char* type = doc["type"];
  if (!type) return;

  if (strcmp(type, "heartbeat") == 0) {
    return;                                   // 只用于保活，不改变显示
  }

  if (strcmp(type, "state") == 0) {
    // 电脑说「现在是某某状态」。status 的取值见设计文档 3.3。
    const char* status = doc["status"];
    if (!status) return;
    // msg 是跟在标题下面那行小字；没有就用空字符串。
    // （`|` 是 ArduinoJson 的写法，意思是「取不到就给默认值」。）
    const String msg = doc["msg"] | "";

    // 收到 state 说明审批流程已经结束，清掉 request_id：
    // 此后误触按钮不会再送出决策，避免「幽灵批准」。
    currentRequestId = "";

    // 把字符串形式的状态翻译成代码里的枚举值。
    // （这里没有 needs_you —— 它只由 approval_request 触发，电脑端不会
    //   用 state 消息发它。）
    if      (strcmp(status, "idle")    == 0) setState(ST_IDLE, msg);
    else if (strcmp(status, "working") == 0) setState(ST_WORKING, msg);
    else if (strcmp(status, "done")    == 0) setState(ST_DONE, msg);
    else if (strcmp(status, "error")   == 0) setState(ST_ERROR, msg);
    else Serial.printf("[vibepet] 未知状态: %s\n", status);
    return;
  }

  if (strcmp(type, "approval_request") == 0) {
    // 「有东西等你批准」—— 最关键的一条。
    // request_id 先存下来：等下按钮回传时要原样带回去，电脑端靠它认领。
    currentRequestId = String(doc["request_id"] | "");
    String summary = String(doc["summary"] | "");
    String tool    = String(doc["tool"] | "");
    // 屏幕上显示成「工具名: 命令摘要」，比如 "Bash: rm -rf 某目录"。
    String shown   = tool.length() ? (tool + ": " + summary) : summary;

    setState(ST_NEEDS_YOU, shown);               // 切成黄框审批卡
    beep(1);                                     // 提示音，把人从别的窗口叫回来
    return;
  }

  Serial.printf("[vibepet] 未知消息类型: %s\n", type);
}

// ════════════════════════════ 按钮 ════════════════════════════
//
// 主循环每转一圈就扫一下这两个按钮，谁被按了就把决定发回电脑。

// 把「按了哪个按钮」发回电脑端。
void sendButton(const char* action) {
  // 蓝牙没连上就发不出去，直接返回 —— 不耽误事，因为屏幕上已经本地切换过了。
  if (!bleConnected || txChar == nullptr) return;

  // 拼一条 JSON：{"type":"button","request_id":"...","action":"approve"}
  StaticJsonDocument<128> doc;
  doc["type"]       = "button";
  doc["request_id"] = currentRequestId;   // ← 原样带回，电脑端靠它认领
  doc["action"]     = action;

  String payload;
  serializeJson(doc, payload);
  payload += "\n";                        // 协议：一条消息占一行

  // 通过蓝牙的「通知」通道发出去。
  txChar->setValue((const uint8_t*)payload.c_str(), payload.length());
  txChar->notify();
  Serial.printf("[vibepet] 按钮已发出: %s (request_id=%s)\n",
                action, currentRequestId.c_str());
}

void scanButtons() {
  // 200 ms 时间戳去抖。主循环往返远快于此，不会漏检。
  //
  // digitalRead 读到 LOW 就说明按钮被按下了。为什么？因为按钮是「一端接引脚、
  // 另一端接地」，而引脚开了内部上拉：没人按时它被上拉电阻拉到高电平，
  // 一按就被接到地上，于是变成低电平。
  if (digitalRead(PIN_BTN_APPROVE) == LOW && millis() - lastApproveMs > DEBOUNCE_MS) {
    lastApproveMs = millis();                  // 记下这次认下的时刻（去抖就靠它）
    if (currentRequestId.length() > 0) {       // 没有待审批请求时不发无意义的包
      sendButton("approve");
      // 本地立即切画面并作废 request_id：不再等电脑端回话才脱离 APPROVE?
      // （BLE 抖动时那里会一直卡着），长按也不会每 200 ms 重复发包。
      currentRequestId = "";
      setState(ST_WORKING, "已批准");
      beep(2, 35);                             // 「嘀嘀」两下短音 = 批准
    }
  }
  if (digitalRead(PIN_BTN_DENY) == LOW && millis() - lastDenyMs > DEBOUNCE_MS) {
    lastDenyMs = millis();
    if (currentRequestId.length() > 0) {
      sendButton("deny");
      currentRequestId = "";
      setState(ST_IDLE, "已拒绝");
      beep(1, 120);                            // 一声长音 = 拒绝
    }
  }
}

// ════════════════════════════ 看门狗 ════════════════════════════
//
// 「看门狗」是个经典叫法：专门安排一个东西盯着，该来的信号一旦没来就报警。
// 这里的信号是电脑端每秒一次的心跳，报警方式是屏幕切成橙色 LOST。
//
// 为什么需要它？因为蓝牙断掉时，我们这边**不会**收到任何通知 —— 只能靠
// 「怎么一直没消息」来推断。没有它的话，屏幕会永远停在最后一个状态上骗你：
// 你以为 AI 还在干活，其实连接早就断了。

void checkWatchdog() {
  // 距离上次收到消息，超过 5 秒了吗？
  bool isLost = (millis() - lastHeartbeat) > WATCHDOG_TIMEOUT;

  if (isLost && !lostShown) {
    // 刚超时（!lostShown 表示之前还没切过）
    lostShown = true;
    currentState = ST_LOST;      // 直接切，不走 setState：失联期间不该保留旧文案
    statusMsg = "";
    currentRequestId = "";       // 此时按钮回传也送不出去，作废以防恢复后幽灵批准
    renderState();
    Serial.println("[vibepet] 心跳超时，进入 LOST");
  } else if (!isLost && lostShown) {
    // 心跳恢复了。切回失联前的最后有效状态。若那是审批请求，恢复成 idle 而不是
    // APPROVE? —— 请求可能已被电脑端判超时，显示过期卡片会误导用户按下无效按钮。
    lostShown = false;
    if (lastValidState == ST_NEEDS_YOU) setState(ST_IDLE, "");
    else                                setState(lastValidState, lastValidMsg);
    Serial.println("[vibepet] 心跳恢复");
  }
  // 其余情况（一直好好的、或一直在失联）什么都不用做。
}

// ════════════════════════════ BLE ════════════════════════════
//
// 这一区是设备端的「耳朵和嘴」：收电脑端发来的消息，也把按钮决定送回去。
//
// 前面导读里说的「两个世界」在这里最明显 —— 下面这几个回调函数**不是**
// 你的 loop() 调的，而是系统在另一个任务里替你调的。所以它们只干最轻的活
// （切行、投队列），绝不干慢活。

// 行重组缓冲。放在文件级而不是回调内的函数静态量，是为了让 onDisconnect 也能
// 复位它：断线时若正好停在半截行上，那截残片会在重连后与第一条消息拼在一起
// 变成非法 JSON，白白丢掉一条。两者都在 BLE 任务里跑，不需要额外加锁。
String rxLineBuffer;
bool   rxDropping = false;

// 蓝牙「连上了 / 断开了」时由系统调用 —— 也就是电脑端的 daemon 连上或断开我们。
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;                      // 让主循环也知道连上了
    digitalWrite(PIN_LED_STATUS, HIGH);       // 点亮指示灯，肉眼可见
    Serial.println("[vibepet] 中心设备已连接");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    digitalWrite(PIN_LED_STATUS, LOW);
    rxLineBuffer = "";                      // 丢掉半截行，否则重连后它会把
    rxDropping   = false;                   // 第一条消息拼成非法 JSON（见变量声明处）
    // 断开后重新广播，等 daemon 重连
    NimBLEDevice::startAdvertising();
    Serial.println("[vibepet] 中心设备已断开，重新广播");
  }
};

// 电脑端往我们这儿写数据时，系统调这个。
//
// ⚠️ 记住它跑在 BLE 任务里、和 loop() 是并发的。所以这里**只做两件事**：
//   ① 按换行符把数据切成一条条完整消息（蓝牙一次给的可能只是半条）；
//   ② 把切好的条子丢进队列，立刻返回。
// 费时的 JSON 解析留给主循环做 —— 免得这边占着 BLE 任务不放。
class RxCallbacks : public NimBLECharacteristicCallbacks {
  // 只做切分与投递。解析放在 loop()，避免与主循环并发操作 String。
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = pChar->getValue();    // 这次收到的原始字节
    for (size_t i = 0; i < value.size(); i++) {  // 一个字节一个字节地过
      char c = (char)value[i];
      if (c == '\n') {
        // 碰到换行 = 一条消息到头了
        if (rxDropping) {
          rxDropping = false;             // 超长行的结尾：丢弃模式复位
          Serial.println("[vibepet] 超长行已丢弃");
        } else if (rxLineBuffer.length() > 0 && lineQueue != nullptr) {
          // 把攒好的这一行搬进队列，交给主循环去解析
          char buf[LINE_MAX];
          rxLineBuffer.toCharArray(buf, LINE_MAX);
          // 队列满就丢弃这条，不阻塞 BLE 任务
          if (xQueueSend(lineQueue, buf, 0) != pdTRUE) {
            Serial.println("[vibepet] 队列已满，丢弃一条消息");
          }
        }
        rxLineBuffer = "";                // 这行处理完了，清空重来
      } else if (rxDropping) {
        // 丢弃本行剩余字节，等 '\n' 复位。不能清空后继续累积 ——
        // 那会把半截尾巴当成一条完整消息投递进队列。
      } else if (rxLineBuffer.length() < LINE_MAX - 1) {
        rxLineBuffer += c;                // 还没到换行，先攒着
      } else {
        // 攒到 LINE_MAX 还没见换行 —— 这行太长了，协议规定整条丢弃。
        // 但不能就这么接着攒下一条（本行的剩余字节还在后面），
        // 所以进入「丢弃模式」：一路扔到遇见换行符为止。
        rxLineBuffer = "";                // 超长行整条丢弃，防止内存被撑爆
        rxDropping = true;
        Serial.println("[vibepet] 行超长，丢弃至行尾");
      }
    }
  }
};

// 把蓝牙服务搭起来：定义两条通道（一收一发），然后开始广播自己。
// 只在 setup() 里调一次。
void setupBle() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(185);              // 摘要 + 头部约 150 字节，一次写得下

  // 建服务、建两条通道。通道用的 UUID 必须和电脑端一致，见文件开头的协议常量。
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(NUS_SERVICE_UUID);

  // 接收通道：电脑往这儿写。不用保存它的引用，挂上回调就行。
  NimBLECharacteristic* rxChar = service->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  // 发送通道：我们往这儿写（在 sendButton 里用），所以要存进全局变量 txChar。
  txChar = service->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  service->start();

  // 开始广播自己 —— 这样电脑端的 daemon 才扫描得到我们。
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setName(DEVICE_NAME);
  adv->start();

  Serial.printf("[vibepet] BLE 已启动，广播名 %s\n", DEVICE_NAME);
}

// ════════════════════════════ setup / loop ════════════════════════════
//
// 这两个就是导读里说的「程序的全部入口」：
//   setup() 开机跑一次，做准备工作；
//   loop()  之后无限循环，一直转。

void setup() {
  // 打开串口，方便从电脑上看设备在嘀咕什么（115200 是波特率）。
  Serial.begin(115200);
  delay(200);                             // 等串口就绪，否则开头几行日志会丢
  Serial.println("\n[vibepet] 启动中…");

  // ── 配置引脚 ──
  // 两个按钮都开 INPUT_PULLUP（启用芯片内部的「上拉电阻」）：不按时引脚是
  // 高电平、按下才变低 —— 这样按钮另一端直接接地就行，不用外接电阻。
  pinMode(PIN_BTN_APPROVE, INPUT_PULLUP);
  pinMode(PIN_BTN_DENY,    INPUT_PULLUP);
  pinMode(PIN_BUZZER,      OUTPUT);
  pinMode(PIN_LED_STATUS,  OUTPUT);
  // 蜂鸣器先设成「不该响」的那个电平，免得一上电就长鸣。
  // 哪个电平算「不该响」要看是有源还是无源（见 BUZZER_ACTIVE 处的说明）。
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, HIGH);         // 低电平触发模块：空闲保持高，否则上电即长鸣
#else
  digitalWrite(PIN_BUZZER, LOW);          // 无源：静止态，无直流偏置
#endif
  digitalWrite(PIN_LED_STATUS, LOW);      // 指示灯先灭着

  // ── 点亮屏幕 ──
  tft.init();
  tft.setRotation(1);                     // 横屏：160×128
  tft.fillScreen(C_BG);

  // 中文字体渲染器绑定屏幕。透明模式（1）：只画字形像素，不铺背景色块
  u8f.begin(tft);
  u8f.setFontMode(1);
  u8f.setFontDirection(0);

  // 开机自检画面，也能用来确认屏幕方向和颜色是否正常
  drawCentered("VibePet", 44, 3, TFT_WHITE);
  drawCentered("starting...", 84, 1, TFT_DARKGREY);

  // ── 建那条「投递通道」（消息队列）──
  // 参数：最多排 8 条，每条最长 512 字节。
  lineQueue = xQueueCreate(LINE_QUEUE_DEPTH, LINE_MAX);
  if (lineQueue == nullptr) {
    Serial.println("[vibepet] 队列创建失败");
  }

  // ── 开蓝牙 ──
  setupBle();

  lastHeartbeat = millis();               // 给看门狗一个起点，免得刚开机就被判失联
  setState(ST_IDLE, "");                  // 显示 IDLE，正式开工
  Serial.println("[vibepet] 就绪");
}

// 主循环：以毫秒级的速度一轮一轮地转，每轮做四件事。
// （顺序有讲究，各条注释里说了为什么。）
void loop() {
  // 1. 消费 BLE 消息
  //    每轮只取一条，取到就解析；没取到就跳过。队列里堆着的消息会在
  //    后面几轮陆续取完（所以一轮取几条并不重要）。
  if (lineQueue != nullptr) {
    char line[LINE_MAX];
    if (xQueueReceive(lineQueue, line, 0) == pdTRUE) {  // 0 = 不等待，有就拿走
      processLine(line);
    }
  }

  // 2. 看门狗（必须在渲染动画之前，失联时不该继续画旧状态）
  checkWatchdog();

  // 3. 按钮
  scanButtons();

  // 4. 动画
  //    分两种情况：审批卡是「慢闪」（每 500 毫秒换一次边框，为了醒目），
  //    其它状态是「快动」（每 50 毫秒重画一次图标，看起来是连续动画）。
  //    判断方式还是那个老套路：算算距离上次画隔了多久。
  unsigned long now = millis();
  if (currentState == ST_NEEDS_YOU) {
    if (now - lastBlinkMs >= BLINK_INTERVAL) {
      lastBlinkMs = now;
      updateAnimation();
    }
  } else if (now - lastAnimMs >= ANIM_INTERVAL) {
    lastAnimMs = now;
    updateAnimation();
  }

  // 5. 最后歇一小会儿（5 毫秒），然后从头再来一轮。
  //    这个 delay 很重要：不歇的话 loop 会以极快的速度空转、白费电，
  //    而且上面那些「隔多久才做一次」的判断也会变得又密又没必要。
  delay(5);
}
