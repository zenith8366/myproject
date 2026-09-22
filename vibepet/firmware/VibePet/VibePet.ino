/*
 * VibePet 设备端固件
 * AI 编程助手物理状态显示与审批终端 —— ESP32-C3 SuperMini + 1.77" ST7735S TFT
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

// —— 引脚（设计文档 4.2 / 4.3，已避开 GPIO9 启动引脚）——
#define PIN_BTN_APPROVE  1
#define PIN_BTN_DENY     10
#define PIN_BUZZER       3
#define PIN_LED_STATUS   2

// 蜂鸣器类型 —— 本项目选型是无源（设计文档 4.3）：
//   0 = 无源蜂鸣器：内部没有振荡源，必须靠 PWM 方波驱动才出声。给恒定
//       直流电平只会听到一声「咔哒」，随后无声。
//   1 = 有源蜂鸣器模块（「3 针低电平触发」那类）：内部自带振荡源，给恒定
//       电平就持续发声，因此空闲必须保持高电平，否则上电即长鸣。
#define BUZZER_ACTIVE 0

// 无源蜂鸣器的驱动频率（Hz），2700 接近常见无源蜂鸣器的谐振点，最响。
#define BUZZER_TONE_HZ 2700

// —— BLE 协议（设计文档 3.3）——
#define DEVICE_NAME      "VibePet"
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // 电脑 → 设备
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // 设备 → 电脑

// —— 时序（设计文档 2.2 / 5.4）——
const unsigned long WATCHDOG_TIMEOUT = 5000;  // 心跳超时 → 显示 LOST
const unsigned long DEBOUNCE_MS      = 200;   // 按钮去抖
const unsigned long ANIM_INTERVAL    = 50;    // 动画帧间隔（20 fps）
const unsigned long BLINK_INTERVAL   = 500;   // APPROVE? 边框闪烁半周期

// —— 画布 ——
// 1.77" ST7735S 原生 128×160，setRotation(1) 后横屏为 160×128。
#define SCREEN_W 160
#define SCREEN_H 128

// 动画只重绘这块区域，其余部分保持不动，避免整屏闪烁
#define ICON_CX 80
#define ICON_CY 36
#define ICON_R  22

// —— 消息队列 ——
// BLE 回调运行在 BLE 任务，解析在主循环，两者不能直接共享 String。
// 512 字节：电脑端单字段（summary/msg）上限 240 字节，带中文的
// approval_request 整行约 330 字节，留足余量；队列 8×512 = 4 KB。
#define LINE_MAX         512
#define LINE_QUEUE_DEPTH 8

// ════════════════════════════ 全局状态 ════════════════════════════

enum PetState { ST_IDLE, ST_WORKING, ST_NEEDS_YOU, ST_DONE, ST_ERROR, ST_LOST };

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;        // 正文的中文字体渲染（TFT_eSPI 内置字体只有 ASCII）
NimBLECharacteristic* txChar = nullptr;
QueueHandle_t lineQueue = nullptr;

volatile bool bleConnected = false;

PetState currentState = ST_IDLE;
String statusMsg      = "";   // error 的错误信息 / needs_you 的命令摘要
String currentRequestId = ""; // 仅在有审批请求时非空

// 失联恢复用：最近一个非 LOST 状态及其文案。看门狗把画面改成 ST_LOST 前
// 先记在这里，心跳恢复时切回去（而不是像以前那样重画一遍 LOST）。
PetState lastValidState = ST_IDLE;
String lastValidMsg = "";

unsigned long lastHeartbeat = 0;
bool lostShown = false;

unsigned long lastApproveMs = 0;
unsigned long lastDenyMs    = 0;

unsigned long lastAnimMs  = 0;
unsigned long lastBlinkMs = 0;
bool blinkOn = true;
float animPhase = 0.0f;

// 颜色别名（六种状态的主色，见设计文档 3.3）
#define C_BG    TFT_BLACK
#define C_IDLE  TFT_WHITE
#define C_WORK  TFT_GREEN
#define C_NEED  TFT_YELLOW
#define C_DONE  TFT_CYAN
#define C_ERR   TFT_RED
#define C_LOST  TFT_ORANGE

// ════════════════════════════ 显示辅助 ════════════════════════════

void drawCentered(const char* text, int y, uint8_t size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  int w = tft.textWidth(text);
  int x = (SCREEN_W - w) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(text);
}

// 粗线：沿垂直方向偏移画多条，否则 1px 的对勾/叉号在小屏上太细
void thickLine(int x0, int y0, int x1, int y1, uint16_t color, int w = 3) {
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  float nx = -dy / len, ny = dx / len;
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
  out.reserve(src.length());
  for (size_t i = 0; i < src.length(); i++) {
    uint8_t c = (uint8_t)src[i];
    out += (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
  }
  return out;
}

// 返回 s[i] 起始的 UTF-8 码点字节数（1~4）；非法前缀字节按 1 处理
int utf8Step(const String& s, int i) {
  uint8_t c = (uint8_t)s[i];
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 按屏宽折行输出，最多 maxLines 行；放不下时末行以 ".." 收尾
void drawWrapped(const String& raw, int y, uint16_t color, int maxLines) {
  const String text = cleanControl(raw);
  u8f.setFont(FONT_BODY);
  u8f.setForegroundColor(color);

  const int maxWidth = SCREEN_W - 8;
  const int lineHeight = u8f.getFontAscent() - u8f.getFontDescent() + 2;
  const int len = (int)text.length();

  // 从 start 起贪心取最长的、渲染宽度不超过 limit 的 UTF-8 前缀长度。
  // 文本很短（≤240 字节），逐字符测宽的平方复杂度只在状态切换时跑一次。
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
    u8f.drawUTF8(4, baseline, chunk.c_str());
    if (line == maxLines - 1 && pos + take < len) {
      u8f.drawUTF8(4 + u8f.getUTF8Width(chunk.c_str()), baseline, "..");
    }
    pos += take;
    line++;
  }
}

// ════════════════════════════ 六种状态绘制 ════════════════════════════

void clearIconArea() {
  tft.fillRect(ICON_CX - ICON_R, ICON_CY - ICON_R, ICON_R * 2, ICON_R * 2, C_BG);
}

void drawIdleIcon() {
  clearIconArea();
  int r = 8 + (int)(5.0f * (1.0f + sinf(animPhase)));  // 半径在 8~18 之间呼吸
  tft.fillCircle(ICON_CX, ICON_CY, r, C_IDLE);
}

void drawWorkingIcon() {
  clearIconArea();
  const int half = 13;
  float pts[4][2];
  for (int i = 0; i < 4; i++) {
    float a = animPhase + i * (PI / 2.0f);
    pts[i][0] = ICON_CX + half * cosf(a);
    pts[i][1] = ICON_CY + half * sinf(a);
  }
  tft.fillTriangle((int)pts[0][0], (int)pts[0][1], (int)pts[1][0], (int)pts[1][1],
                   (int)pts[2][0], (int)pts[2][1], C_WORK);
  tft.fillTriangle((int)pts[0][0], (int)pts[0][1], (int)pts[2][0], (int)pts[2][1],
                   (int)pts[3][0], (int)pts[3][1], C_WORK);
}

void drawDoneIcon() {
  clearIconArea();
  thickLine(ICON_CX - 16, ICON_CY + 1, ICON_CX - 4, ICON_CY + 13, C_DONE);
  thickLine(ICON_CX - 4, ICON_CY + 13, ICON_CX + 17, ICON_CY - 11, C_DONE);
}

void drawErrorIcon() {
  clearIconArea();
  thickLine(ICON_CX - 13, ICON_CY - 13, ICON_CX + 13, ICON_CY + 13, C_ERR);
  thickLine(ICON_CX + 13, ICON_CY - 13, ICON_CX - 13, ICON_CY + 13, C_ERR);
}

// 各状态的整屏渲染。状态切换时调用一次，之后由动画帧局部更新。
void renderState() {
  tft.fillScreen(C_BG);

  switch (currentState) {
    case ST_IDLE:
      drawIdleIcon();
      drawCentered("IDLE", 70, 2, C_IDLE);
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
      tft.fillScreen(C_LOST);
      drawCentered("LOST", 48, 3, TFT_BLACK);
      drawCentered("no heartbeat", 88, 1, TFT_BLACK);
      break;
  }
}

// 动画帧：只更新会动的部分
void updateAnimation() {
  switch (currentState) {
    case ST_IDLE:
      animPhase += 0.16f;
      drawIdleIcon();
      break;

    case ST_WORKING:
      animPhase += 0.22f;
      drawWorkingIcon();
      break;

    case ST_NEEDS_YOU:
      // 边框闪烁提示「在等你」，只重画四条边，不动文字
      blinkOn = !blinkOn;
      tft.drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, blinkOn ? C_NEED : C_BG);
      tft.drawRect(2, 2, SCREEN_W - 4, SCREEN_H - 4, blinkOn ? C_NEED : C_BG);
      break;

    default:
      break;  // done / error / lost 是静态画面，没有动画
  }
}

// ════════════════════════════ 蜂鸣器 ════════════════════════════

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

// 阻塞式发声。loop 被挡住不影响收包 —— BLE 回调在独立任务里，
// 消息会堆在队列中，这里返回后再消费。
void beep(int times, unsigned long ms = 60) {
  for (int i = 0; i < times; i++) {
    beepOnce(ms);
    if (i < times - 1) delay(70);
  }
}

// ════════════════════════════ 状态切换 ════════════════════════════

void setState(PetState next, const String& msg = "") {
  if (next != ST_LOST) {           // LOST 是看门狗临时覆盖的画面，不算「有效状态」
    lastValidState = next;
    lastValidMsg = msg;
  }
  currentState = next;
  statusMsg = msg;
  animPhase = 0.0f;
  blinkOn = true;
  renderState();
}

// ════════════════════════════ 消息处理 ════════════════════════════

void processLine(const char* line) {
  // ArduinoJson 7 里 StaticJsonDocument<N> 只是 JsonDocument 的兼容壳：池是动态的
  // （堆分配、按需增长），<512> 既不限制也不预留内存，别把它当成 v6 那种
  // 「栈上定长池」的安全保证。真正的内存上限来自协议 —— 整行 ≤ LINE_MAX(512)
  // 字节，见设计文档 3.3 与 pc/bridge_daemon.py 的 MAX_FIELD_BYTES。
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) {
    Serial.printf("[vibepet] JSON 解析失败，已忽略: %s\n", line);
    return;
  }

  // 任何一条合法消息都算保活信号（不只是 heartbeat 包）。
  // 设备端是这么设计的：daemon 断线后不再有任何消息，看门狗才超时。
  lastHeartbeat = millis();

  const char* type = doc["type"];
  if (!type) return;

  if (strcmp(type, "heartbeat") == 0) {
    return;                                   // 只用于保活，不改变显示
  }

  if (strcmp(type, "state") == 0) {
    const char* status = doc["status"];
    if (!status) return;
    const String msg = doc["msg"] | "";

    // 收到 state 说明审批流程已经结束，清掉 request_id：
    // 此后误触按钮不会再送出决策，避免「幽灵批准」。
    currentRequestId = "";

    if      (strcmp(status, "idle")    == 0) setState(ST_IDLE, msg);
    else if (strcmp(status, "working") == 0) setState(ST_WORKING, msg);
    else if (strcmp(status, "done")    == 0) setState(ST_DONE, msg);
    else if (strcmp(status, "error")   == 0) setState(ST_ERROR, msg);
    else Serial.printf("[vibepet] 未知状态: %s\n", status);
    return;
  }

  if (strcmp(type, "approval_request") == 0) {
    currentRequestId = String(doc["request_id"] | "");
    String summary = String(doc["summary"] | "");
    String tool    = String(doc["tool"] | "");
    String shown   = tool.length() ? (tool + ": " + summary) : summary;

    setState(ST_NEEDS_YOU, shown);
    beep(1);                                   // 提示音，把人从别的窗口叫回来
    return;
  }

  Serial.printf("[vibepet] 未知消息类型: %s\n", type);
}

// ════════════════════════════ 按钮 ════════════════════════════

void sendButton(const char* action) {
  if (!bleConnected || txChar == nullptr) return;

  StaticJsonDocument<128> doc;
  doc["type"]       = "button";
  doc["request_id"] = currentRequestId;
  doc["action"]     = action;

  String payload;
  serializeJson(doc, payload);
  payload += "\n";

  txChar->setValue((const uint8_t*)payload.c_str(), payload.length());
  txChar->notify();
  Serial.printf("[vibepet] 按钮已发出: %s (request_id=%s)\n",
                action, currentRequestId.c_str());
}

void scanButtons() {
  // 200 ms 时间戳去抖。主循环往返远快于此，不会漏检。
  if (digitalRead(PIN_BTN_APPROVE) == LOW && millis() - lastApproveMs > DEBOUNCE_MS) {
    lastApproveMs = millis();
    if (currentRequestId.length() > 0) {       // 没有待审批请求时不发无意义的包
      sendButton("approve");
      // 本地立即切画面并作废 request_id：不再等电脑端回话才脱离 APPROVE?
      // （BLE 抖动时那里会一直卡着），长按也不会每 200 ms 重复发包。
      currentRequestId = "";
      setState(ST_WORKING, "已批准");
      beep(2, 35);
    }
  }
  if (digitalRead(PIN_BTN_DENY) == LOW && millis() - lastDenyMs > DEBOUNCE_MS) {
    lastDenyMs = millis();
    if (currentRequestId.length() > 0) {
      sendButton("deny");
      currentRequestId = "";
      setState(ST_IDLE, "已拒绝");
      beep(1, 120);
    }
  }
}

// ════════════════════════════ 看门狗 ════════════════════════════

void checkWatchdog() {
  bool isLost = (millis() - lastHeartbeat) > WATCHDOG_TIMEOUT;

  if (isLost && !lostShown) {
    lostShown = true;
    currentState = ST_LOST;      // 直接切，不走 setState：失联期间不该保留旧文案
    statusMsg = "";
    currentRequestId = "";       // 此时按钮回传也送不出去，作废以防恢复后幽灵批准
    renderState();
    Serial.println("[vibepet] 心跳超时，进入 LOST");
  } else if (!isLost && lostShown) {
    // 心跳恢复：切回失联前的最后有效状态。若那是审批请求，恢复成 idle 而不是
    // APPROVE? —— 请求可能已被电脑端判超时，显示过期卡片会误导用户按下无效按钮。
    lostShown = false;
    if (lastValidState == ST_NEEDS_YOU) setState(ST_IDLE, "");
    else                                setState(lastValidState, lastValidMsg);
    Serial.println("[vibepet] 心跳恢复");
  }
}

// ════════════════════════════ BLE ════════════════════════════

// 行重组缓冲。放在文件级而不是回调内的函数静态量，是为了让 onDisconnect 也能
// 复位它：断线时若正好停在半截行上，那截残片会在重连后与第一条消息拼在一起
// 变成非法 JSON，白白丢掉一条。两者都在 BLE 任务里跑，不需要额外加锁。
String rxLineBuffer;
bool   rxDropping = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    digitalWrite(PIN_LED_STATUS, HIGH);
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

class RxCallbacks : public NimBLECharacteristicCallbacks {
  // 只做切分与投递。解析放在 loop()，避免与主循环并发操作 String。
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue value = pChar->getValue();
    for (size_t i = 0; i < value.size(); i++) {
      char c = (char)value[i];
      if (c == '\n') {
        if (rxDropping) {
          rxDropping = false;             // 超长行的结尾：丢弃模式复位
          Serial.println("[vibepet] 超长行已丢弃");
        } else if (rxLineBuffer.length() > 0 && lineQueue != nullptr) {
          char buf[LINE_MAX];
          rxLineBuffer.toCharArray(buf, LINE_MAX);
          // 队列满就丢弃这条，不阻塞 BLE 任务
          if (xQueueSend(lineQueue, buf, 0) != pdTRUE) {
            Serial.println("[vibepet] 队列已满，丢弃一条消息");
          }
        }
        rxLineBuffer = "";
      } else if (rxDropping) {
        // 丢弃本行剩余字节，等 '\n' 复位。不能清空后继续累积 ——
        // 那会把半截尾巴当成一条完整消息投递进队列。
      } else if (rxLineBuffer.length() < LINE_MAX - 1) {
        rxLineBuffer += c;
      } else {
        rxLineBuffer = "";                // 超长行整条丢弃，防止内存被撑爆
        rxDropping = true;
        Serial.println("[vibepet] 行超长，丢弃至行尾");
      }
    }
  }
};

void setupBle() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(185);              // 摘要 + 头部约 150 字节，一次写得下

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(NUS_SERVICE_UUID);

  NimBLECharacteristic* rxChar = service->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());

  txChar = service->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  service->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setName(DEVICE_NAME);
  adv->start();

  Serial.printf("[vibepet] BLE 已启动，广播名 %s\n", DEVICE_NAME);
}

// ════════════════════════════ setup / loop ════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[vibepet] 启动中…");

  pinMode(PIN_BTN_APPROVE, INPUT_PULLUP);
  pinMode(PIN_BTN_DENY,    INPUT_PULLUP);
  pinMode(PIN_BUZZER,      OUTPUT);
  pinMode(PIN_LED_STATUS,  OUTPUT);
#if BUZZER_ACTIVE
  digitalWrite(PIN_BUZZER, HIGH);         // 低电平触发模块：空闲保持高，否则上电即长鸣
#else
  digitalWrite(PIN_BUZZER, LOW);          // 无源：静止态，无直流偏置
#endif
  digitalWrite(PIN_LED_STATUS, LOW);

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

  lineQueue = xQueueCreate(LINE_QUEUE_DEPTH, LINE_MAX);
  if (lineQueue == nullptr) {
    Serial.println("[vibepet] 队列创建失败");
  }

  setupBle();

  lastHeartbeat = millis();
  setState(ST_IDLE, "");
  Serial.println("[vibepet] 就绪");
}

void loop() {
  // 1. 消费 BLE 消息
  if (lineQueue != nullptr) {
    char line[LINE_MAX];
    if (xQueueReceive(lineQueue, line, 0) == pdTRUE) {
      processLine(line);
    }
  }

  // 2. 看门狗（必须在渲染动画之前，失联时不该继续画旧状态）
  checkWatchdog();

  // 3. 按钮
  scanButtons();

  // 4. 动画
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

  delay(5);
}
