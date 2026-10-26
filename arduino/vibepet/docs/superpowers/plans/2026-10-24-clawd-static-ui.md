# Clawd 静态画面重设计 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除固件全部逐帧动画，用 Claude 吉祥物 Clawd 的珊瑚橙像素造型重做竖屏静态画面。

**Architecture:** 保留状态机、协议、按钮、蜂鸣器、看门狗；只重写绘制层。`renderState()` 在状态切换时整屏重绘一次，`loop()` 不再调用动画函数。

**Tech Stack:** Arduino UNO R3 / arduino-cli / Adafruit_ST7735 / 128×160 竖屏

## Global Constraints

- 设备端任何连续阻塞 ≤ 3 ms；禁用 `delay()`、`String`、动态内存、浮点。
- 编译必须带 `-DSERIAL_RX_BUFFER_SIZE=256` 与 `--build-path`。
- 烧录前必须停 daemon；hook 目前处于临时注释状态，联调后恢复启用。
- 降级方向一律是拒绝；交互类工具例外（直接 allow）。
- Flash 上限 32256 B；目标留出 ≥ 300 B 余量。
- 收尾清理临时文件；文档同步 `CLAUDE.md`、`README.md`、设计文档 v3.0。

## File Structure

- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino` —— 常量、颜色、绘制函数、`renderState`、删除动画
- Modify: `CLAUDE.md`、`README.md`、`VibePet …… v3.0.md` —— 静态画面与 Clawd 描述
- Create: `docs/superpowers/specs/2026-10-24-clawd-static-ui-design.md`（已完成）
- Create: `docs/superpowers/plans/2026-10-24-clawd-static-ui.md`（本文件）

---

## Task 1: 固件静态画面重写

**Files:**
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino`

**Interfaces:**
- Consumes: `fillScreenSliced`、`printTitle`、`bodyLine`、`drawGlyph`、`drawText`、`thickLine`、`pumpSerial`
- Produces: `drawClawdBody()`、`drawEyesOpen/Half/Wide/Happy/X/Closed()`、
  `drawPropBang/Check/Cloud/Lost/Monitor()`、`drawLaptop()`、`drawBubbleFrame()`、
  `drawBubbleTail()`、`drawBodyText(const char*)`

- [ ] **Step 1: 替换时序与布局常量**

把当前的时序 + 布局常量整段（从 `// ── 时序（毫秒）──` 到 `#define HEADER_DOT_Y 12`）替换为：

```c
// ── 时序（毫秒）──
#define WATCHDOG_TIMEOUT 5000   // 超过这么久没收到任何消息 → 显示「失联」
#define BUTTON_STABLE_MS   35   // 按下电平连续稳定这么久才算一次有效按键
#define DONE_HOLD_MS     3000   // DONE 停留 3 秒后自动回 IDLE

// ── 屏幕布局（竖屏 128×160，Clawd 静态画面）──
#define SCREEN_W   128
#define SCREEN_H   160
#define TITLE_Y      6
#define TITLE_H     16
#define STATUS_LINE_Y 26
#define BODY_X      35    // Clawd 身体
#define BODY_Y      52
#define BODY_W      58
#define BODY_H      38
#define EYE_Y       60
#define EYE_L_X     45
#define EYE_R_X     73
#define EYE_SIZE     8
#define CLAW_Y      62    // 钳子
#define LEG_Y       90    // 腿
#define LEG_H       16
#define BUBBLE_X     4    // 正文气泡
#define BUBBLE_Y   108
#define BUBBLE_W   120
#define BUBBLE_H    46
#define BUBBLE_PAD   6
#define BODY_WIDTH (BUBBLE_W - 2 * BUBBLE_PAD)   // 108
#define BODY_LINES   3
#define BODY_LINE_H 13
```

删除原来的布局编译期检查（`#if ICON_AREA_TOP ...` 等整段）。

- [ ] **Step 2: 替换颜色区**

把颜色区（从 `// ── 颜色（RGB565）──` 到 `#define DIM(c) ...`）替换为：

```c
// ── 颜色（RGB565）──
// 米白底 + Clawd 经典珊瑚橙；状态色只用于标题、状态线与道具。
#define C_BG       0xFFBC   // #FFF5E2 米白
#define C_INK      0x18A2   // #1A1412 深色文字/眼睛/气泡边框
#define C_CLAWD    0xDBAA   // #D97757 Claude 经典橘
#define C_CLAWD_DK 0xB287   // #B5523A 腿与暗部
#define C_CARD     0xFFFF   // 气泡白底
#define C_IDLE     0xDBAA
#define C_WORKING  0x29EF
#define C_NEEDS    0xF627
#define C_DONE     0x3CF3
#define C_ERROR    0xD9C9
#define C_LOST     0xA558
#define C_INTERACTIVE 0x7AF5
```

- [ ] **Step 3: 清理全局动画状态**

删除以下变量：

```c
static unsigned long lastAnimMs = 0;
static unsigned long lastBlinkMs = 0;
static uint8_t animPhase = 0;
static bool borderOn = false;
static bool compactBody = false;
```

保留 `stateSince`、`approveLowSince` 等按键状态。

- [ ] **Step 4: 用静态绘制替换整个绘制区**

删除从 `// ═══════════════════════════ 五、切片绘制 ═══════════════════════════`
到 `drawLostScreen()` 结束的全部内容（包括 `clearIconArea`、`blend565`、
`lighten565`、`drawHalo`、六个 `draw*Icon`、`drawBorder`、`drawLostSignal`、
`drawLostScreen`），保留 `fillScreenSliced` 与 `thickLine`。替换为：

```c
// ═══════════════════════════ 五、切片绘制与 Clawd ═══════════════════════════

#define SLICE_H 8   // 每片 8 行：约 2.5 毫秒，安全

static void fillScreenSliced(uint16_t color) {
  for (int16_t y = 0; y < SCREEN_H; y += SLICE_H) {
    tft.fillRect(0, y, SCREEN_W, SLICE_H, color);
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

// ── Clawd：2× 放大自官方 48×52 逻辑像素网格 ──
// 身体宽扁、钳子水平伸出、四条细腿；纯色平涂、无描边。

static void drawClawdBody() {
  // 四条腿（先画，身体会盖住根部）
  tft.fillRect(41, LEG_Y, 6, LEG_H, C_CLAWD_DK);
  tft.fillRect(53, LEG_Y, 6, LEG_H, C_CLAWD_DK);
  tft.fillRect(69, LEG_Y, 6, LEG_H, C_CLAWD_DK);
  tft.fillRect(81, LEG_Y, 6, LEG_H, C_CLAWD_DK);
  pumpSerial();

  // 左右钳子：水平细条
  tft.fillRect(23, CLAW_Y, 12, 12, C_CLAWD);
  tft.fillRect(93, CLAW_Y, 12, 12, C_CLAWD);
  pumpSerial();

  // 身体：宽扁圆角矩形
  tft.fillRoundRect(BODY_X, BODY_Y, BODY_W, BODY_H, 4, C_CLAWD);
  pumpSerial();
}

static void drawEyesOpen() {
  tft.fillRect(EYE_L_X, EYE_Y, EYE_SIZE, EYE_SIZE, C_INK);
  tft.fillRect(EYE_R_X, EYE_Y, EYE_SIZE, EYE_SIZE, C_INK);
  pumpSerial();
}

static void drawEyesHalf() {
  tft.fillRect(EYE_L_X, EYE_Y + 4, EYE_SIZE, 4, C_INK);
  tft.fillRect(EYE_R_X, EYE_Y + 4, EYE_SIZE, 4, C_INK);
  pumpSerial();
}

static void drawEyesWide() {
  tft.fillRect(EYE_L_X, EYE_Y - 2, EYE_SIZE, EYE_SIZE + 4, C_INK);
  tft.fillRect(EYE_R_X, EYE_Y - 2, EYE_SIZE, EYE_SIZE + 4, C_INK);
  pumpSerial();
}

static void drawEyesHappy() {
  thickLine(EYE_L_X, EYE_Y + 6, EYE_L_X + 4, EYE_Y + 2, 2, C_INK);
  thickLine(EYE_L_X + 4, EYE_Y + 2, EYE_L_X + 8, EYE_Y + 6, 2, C_INK);
  thickLine(EYE_R_X, EYE_Y + 6, EYE_R_X + 4, EYE_Y + 2, 2, C_INK);
  thickLine(EYE_R_X + 4, EYE_Y + 2, EYE_R_X + 8, EYE_Y + 6, 2, C_INK);
  pumpSerial();
}

static void drawEyesX() {
  thickLine(EYE_L_X, EYE_Y, EYE_L_X + 7, EYE_Y + 7, 2, C_INK);
  thickLine(EYE_L_X, EYE_Y + 7, EYE_L_X + 7, EYE_Y, 2, C_INK);
  thickLine(EYE_R_X, EYE_Y, EYE_R_X + 7, EYE_Y + 7, 2, C_INK);
  thickLine(EYE_R_X, EYE_Y + 7, EYE_R_X + 7, EYE_Y, 2, C_INK);
  pumpSerial();
}

static void drawEyesClosed() {
  tft.fillRect(EYE_L_X, EYE_Y + 4, EYE_SIZE, 3, C_INK);
  tft.fillRect(EYE_R_X, EYE_Y + 4, EYE_SIZE, 3, C_INK);
  pumpSerial();
}

// ── 状态道具（全部在 y=32..50 的道具区）──

static void drawPropBang() {
  tft.fillRect(62, 32, 4, 12, C_NEEDS);
  tft.fillRect(62, 47, 4, 4, C_NEEDS);
  pumpSerial();
}

static void drawPropCheck() {
  thickLine(96, 40, 101, 46, 3, C_DONE);
  thickLine(101, 46, 110, 34, 3, C_DONE);
  tft.drawFastVLine(116, 32, 4, C_DONE);
  tft.drawFastHLine(114, 34, 4, C_DONE);
  pumpSerial();
}

static void drawPropCloud() {
  tft.fillRoundRect(88, 32, 24, 8, 4, C_LOST);
  tft.fillCircle(94, 34, 5, C_LOST);
  tft.fillCircle(104, 33, 6, C_LOST);
  tft.fillRect(92, 44, 2, 4, C_ERROR);
  tft.fillRect(99, 44, 2, 4, C_ERROR);
  tft.fillRect(106, 44, 2, 4, C_ERROR);
  pumpSerial();
}

static void drawPropLost() {
  tft.drawLine(90, 42, 96, 36, C_LOST);
  tft.drawLine(96, 36, 102, 42, C_LOST);
  tft.drawLine(94, 46, 98, 42, C_LOST);
  tft.drawLine(98, 42, 102, 46, C_LOST);
  tft.fillCircle(96, 48, 2, C_LOST);
  tft.drawLine(88, 48, 104, 32, C_INK);
  pumpSerial();
}

static void drawPropMonitor() {
  tft.drawRoundRect(86, 32, 24, 14, 2, C_INTERACTIVE);
  tft.drawFastHLine(94, 46, 8, C_INTERACTIVE);
  tft.drawFastVLine(98, 46, 3, C_INTERACTIVE);
  tft.fillRect(90, 36, 7, 2, C_INTERACTIVE);
  tft.drawLine(112, 40, 118, 40, C_INTERACTIVE);
  tft.drawLine(115, 37, 118, 40, C_INTERACTIVE);
  tft.drawLine(115, 43, 118, 40, C_INTERACTIVE);
  pumpSerial();
}

// 笔记本电脑挡在身体下半，眼睛露在上面（还原 Clawd 的工作姿势）
static void drawLaptop() {
  tft.fillRect(41, 72, 46, 16, C_INK);            // 屏幕外框
  tft.fillRect(43, 74, 42, 12, 0x2965);           // 屏幕深蓝
  tft.fillRect(46, 77, 12, 2, C_DONE);            // 代码行
  tft.fillRect(46, 80, 18, 2, C_DONE);
  tft.fillRect(46, 83, 9, 2, C_DONE);
  tft.fillRect(38, 88, 52, 4, 0x9492);            // 键盘底座
  pumpSerial();
}

// ── 正文气泡 ──

static void drawBubbleFrame() {
  tft.fillRoundRect(BUBBLE_X, BUBBLE_Y, BUBBLE_W, BUBBLE_H, 6, C_CARD);
  tft.drawRoundRect(BUBBLE_X, BUBBLE_Y, BUBBLE_W, BUBBLE_H, 6, C_INK);
  tft.drawRoundRect(BUBBLE_X + 1, BUBBLE_Y + 1,
                    BUBBLE_W - 2, BUBBLE_H - 2, 5, C_INK);
  pumpSerial();
}

static void drawBubbleTail() {
  tft.fillTriangle(58, BUBBLE_Y, 70, BUBBLE_Y, 64, BUBBLE_Y - 6, C_CARD);
  tft.drawLine(58, BUBBLE_Y, 64, BUBBLE_Y - 6, C_INK);
  tft.drawLine(70, BUBBLE_Y, 64, BUBBLE_Y - 6, C_INK);
  pumpSerial();
}
```

- [ ] **Step 5: 重写 `drawBodyText`**

把现有 `drawBodyText(const char *text)` 替换为：

```c
// 在气泡内按字折行：单行居中，多行左对齐，最多 3 行，超出用 .. 收尾。
static void drawBodyText(const char *text) {
  const char *lineStart[BODY_LINES];
  const char *lineEnd[BODY_LINES];
  uint16_t lineWidth[BODY_LINES];
  uint8_t lineCount = 0;
  const char *cursor = text;
  while (*cursor != '\0' && lineCount < BODY_LINES) {
    lineStart[lineCount] = cursor;
    cursor = bodyLine(cursor, &lineEnd[lineCount], &lineWidth[lineCount]);
    while (*cursor == ' ') cursor++;
    lineCount++;
  }

  uint8_t dotsWidth = glyphAdvance('.') * 2;
  for (uint8_t i = 0; i < lineCount; i++) {
    int16_t x = lineCount == 1
                    ? BUBBLE_X + (BUBBLE_W - lineWidth[i]) / 2
                    : BUBBLE_X + BUBBLE_PAD;
    int16_t top = BUBBLE_Y + 8 + i * BODY_LINE_H;
    const char *p = lineStart[i];
    bool truncated = (i + 1 == lineCount) && (*cursor != '\0');
    int16_t right = BUBBLE_X + BUBBLE_W - BUBBLE_PAD -
                    (truncated ? dotsWidth : 0);
    while (p < lineEnd[i]) {
      uint16_t code = utf8Next(&p);
      if (code == 0) break;
      if (x + glyphAdvance(code) > right) break;
      drawGlyph(x, top, code, C_INK, C_CARD);
      x += glyphAdvance(code);
    }
    if (truncated) drawText(x, top, "..", C_INK, C_CARD);
    pumpSerial();
  }
}
```

删除 `needsCompactBody()`。

- [ ] **Step 6: 重写 `renderState`**

```c
static void renderState() {
  fillScreenSliced(C_BG);
  uint16_t color = stateColor(currentState);

  switch (currentState) {
    case ST_WORKING:     printTitle(F("WORKING"), 7, C_INK); break;
    case ST_NEEDS_YOU:   printTitle(F("APPROVE?"), 8, C_INK); break;
    case ST_DONE:        printTitle(F("DONE"), 4, C_INK); break;
    case ST_ERROR:       printTitle(F("ERROR"), 5, C_INK); break;
    case ST_INTERACTIVE: printTitle(F("CHECK PC"), 8, C_INK); break;
    case ST_IDLE:
    default:             printTitle(F("IDLE"), 4, C_INK); break;
  }

  tft.fillRect(8, 12, 6, 6, color);                 // 状态色小方块
  tft.fillRect(16, STATUS_LINE_Y, 96, 2, color);    // 状态色细线
  pumpSerial();

  // 需要人介入的状态：静态双线外框（不闪烁）
  if (currentState == ST_NEEDS_YOU || currentState == ST_INTERACTIVE) {
    tft.drawRect(0, 0, SCREEN_W, SCREEN_H, color);
    tft.drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, color);
    pumpSerial();
  }

  drawClawdBody();
  switch (currentState) {
    case ST_IDLE:        drawEyesOpen(); break;
    case ST_WORKING:     drawEyesHalf(); drawLaptop(); break;
    case ST_NEEDS_YOU:   drawEyesWide(); drawPropBang(); break;
    case ST_DONE:        drawEyesHappy(); drawPropCheck(); break;
    case ST_ERROR:       drawEyesX(); drawPropCloud(); break;
    case ST_LOST:        drawEyesClosed(); drawPropLost(); break;
    case ST_INTERACTIVE: drawEyesOpen(); drawPropMonitor(); break;
  }

  drawBubbleFrame();
  drawBubbleTail();
  if (msgBuf[0] != '\0') drawBodyText(msgBuf);
}
```

- [ ] **Step 7: 删除 `updateAnimation` 与 loop 调用**

删除整个 `updateAnimation()` 函数；删除 `loop()` 里的 `updateAnimation();` 一行。
`loop()` 保留 `checkWatchdog()`、DONE 回退、`scanButtons()`、`beepService()`。

- [ ] **Step 8: 编译并修复**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build firmware/VibePet_UNO 2>&1 \
  | grep -E "error:|warning:|Sketch uses|Global variables"
```

Expected: 零警告；Flash 明显下降（目标 ≤ 31900 B，余量 ≥ 300 B）。

- [ ] **Step 9: 离线回归**

```bash
g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o /tmp/proto_test \
  && /tmp/proto_test
python pc/_smoke_test.py
python pc/_smoke_test_daemon.py
python pc/_smoke_test_state.py
python pc/_smoke_test_serial.py
```

Expected: `55/55`、`12/12`、`14/14`、`14/14`、`9/9`。

---

## Task 2: 文档同步

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: `VibePet —— AI 编程助手物理状态显示与审批终端（USB 有线版）v3.0.md`

- [ ] **Step 1: `CLAUDE.md`**

- 当前状态新增一条：2026-10-24 静态 Clawd 画面重设计，删除全部逐帧动画；
  米白底 + 经典橘 Clawd；记录实际 Flash/RAM。
- 「状态动画更新」条目改为「状态画面」：说明静态绘制、七态道具。
- 硬件接线表 LED 说明保持「失联 / 待审批 / 交互提醒时点亮」。
- 屏幕描述保持 128×160 竖屏。

- [ ] **Step 2: `README.md`**

- 「屏幕上的七种状态」表格保持；增加说明：画面为静态 Clawd 像素形象，
  米白底、珊瑚橙螃蟹，正文在对话气泡里。
- 删掉关于「图标动效」的旧描述，改为静态画面。
- 排障：如果屏幕显示异常，先确认 `setRotation(0)`。

- [ ] **Step 3: 设计文档 v3.0**

- 状态提示效果一节改为静态 Clawd 描述（七态表情与道具）。
- 配色表加入 Claude 品牌色（米白 / 经典橘 / 深色）。

---

## Task 3: 上机联调

**Files:**
- Modify: `CLAUDE.md`（记录实测结果）

> 前置：daemon 可能正在后台运行；烧录前用 TaskStop 停止。

- [ ] **Step 1: 停 daemon 并烧录**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build --upload -p COM9 firmware/VibePet_UNO \
  > /tmp/vibepet_upload3.log 2>&1
"$CLI" upload -p COM9 --fqbn arduino:avr:uno \
  --build-path firmware/VibePet_UNO/build -v firmware/VibePet_UNO 2>&1 | tail -6
```

Expected: `32098` 附近的新字节数写入成功。

- [ ] **Step 2: 启动 daemon 并检查七态**

```bash
python pc/bridge_daemon.py   # 后台
```

用 `tools/serial_probe.py COM9`（先停 daemon）逐个发送：

```text
/state idle
/state working 正在分析代码
/state needs_you 要删缓存了
/state done 任务完成
/state error 出错了
/state interactive 请到电脑答题
```

Expected: 每态静态画面正确；Clawd 宽扁身体、水平钳、四条腿；
米白底与珊瑚橙对比清晰；道具不越界；气泡文字 3 行内。

- [ ] **Step 3: DONE 回退与 LOST**

- `/state done` 后 3 秒回 `IDLE`；
- 停 daemon 5 秒 → `LOST`（闭眼 + 断线符号）+ LED 亮；恢复后正常。

- [ ] **Step 4: 审批与交互提醒**

恢复 hook 启用；Bash 批准/拒绝、`AskUserQuestion` → `CHECK PC`
（静态蓝紫框 + 显示器道具）。

- [ ] **Step 5: 记录结果**

把实测结果写入 `CLAUDE.md`；确认无临时文件残留。

---

## Self-Review

- **Spec coverage:** 全静态 → Task 1 Step 7；Clawd 高还原 → Step 4；米白底与配色 → Step 2；
  七态表情/道具 → Step 4 + Step 6；气泡正文 → Step 4/5；DONE 回退保留 → 已存在；
  文档 → Task 2；上机 → Task 3。
- **Placeholder scan:** 无 TBD；所有绘制函数给出完整代码。
- **Type consistency:** `C_CLAWD` / `C_CLAWD_DK` / `C_CARD` / `C_INK` 在 Step 2 定义、
  Step 4 使用；`BODY_WIDTH` 在 Step 1 定义为 108，`bodyLine` 直接复用。
- **预算风险:** 删除动画后 Flash 应下降；若仍超限，先简化道具（保留 Clawd 主体）。
