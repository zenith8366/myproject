# 竖屏 + 科技动画 + 交互提醒 + DONE 回退 Implementation Plan

> **执行后记（2026-10-27）**：本计划中的竖屏、第七态 `interactive`、`DONE` 回退、
> 交互类工具豁免已经落地；**全部逐帧动画步骤已被同日 Clawd 静态画面方案取代**，
> 不再按本计划恢复动画。当前实现以固件与 `CLAUDE.md` 为准。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 VibePet 改成竖屏 128×160，DONE 3 秒后回 IDLE，交互类工具改为「CHECK PC / 请到电脑答题」提醒动画，并把六态动画升级为科技精致风格。

**Architecture:** 固件仍是单文件协作式调度；新增第七态 `ST_INTERACTIVE`，由 `hook_client.py` 在交互类工具上直接上报并放行。动画增强只增加绘制函数与 PROGMEM 查表，不引入浮点、动态内存或 `delay()`。

**Tech Stack:** Arduino UNO R3 / arduino-cli 1.5.1 / AVR core 1.8.8 / Adafruit_ST7735 / Python 3.14 + pyserial 3.5

## Global Constraints

- 设备端任何一处连续阻塞 ≤ 3 ms；禁用 `delay()`、`String`、动态内存、浮点。
- 固件编译必须带 `-DSERIAL_RX_BUFFER_SIZE=256` 与 `--build-path`。
- 烧录前、打开串口前必须停掉 daemon。
- hook 只改项目级 `.claude/settings.json`；**不得改动用户级全局配置**。
- 降级方向一律是拒绝；唯一例外是交互类工具（`AskUserQuestion` / `ExitPlanMode`）直接 `allow`。
- `hook_client.py` 的 stdout 只能输出 Hook JSON。
- 收尾时清点并删除过程产生的临时文件。
- 用户可见行为变化要同步 `README.md`；项目状态与协议变化要同步 `CLAUDE.md` 与设计文档 v3.0。
- **Flash 硬上限 32256 B**。当前 30474 B，余量 1782 B；超限时按 spec 的裁剪顺序处理。

## File Structure

- Modify: `tools/cn_charset.txt` —— A 组补「电脑答题」
- Regenerate: `firmware/VibePet_UNO/cn_font.h` —— 由 `tools/gen_cn_font.py` 生成
- Modify: `pc/hook_client.py` —— 交互类工具豁免
- Modify: `pc/_smoke_test.py` —— 新增交互工具用例
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino` —— 第七态、DONE 回退、竖屏、动画
- Modify: `CLAUDE.md`、`README.md`、`.claude/README.md`、设计文档 v3.0 —— 文档同步
- Create: `docs/superpowers/specs/2026-10-24-portrait-tech-animation-design.md`（已完成）
- Create: `docs/superpowers/plans/2026-10-24-portrait-tech-animation.md`（本文件）

---

## Task 1: 字库补字

**Files:**
- Modify: `tools/cn_charset.txt:17-20`
- Regenerate: `firmware/VibePet_UNO/cn_font.h`

**Interfaces:**
- Consumes: 无
- Produces: 字库新增「电、脑、答、题」四个字形；`cnFontRow()` 签名不变

- [ ] **Step 1: 把缺字加入 A 组**

把第 17–20 行：

```text
# 设备自身：已批准 / 已拒绝 / 就绪 / 等待 / 失联 / 恢复
# hook_client.py 的状态小字：会话开始 / 会话结束 / 思考中 / 子任务完成 /
#                     压缩上下文 / 任务完成 / <工具> 完成 / <工具> 出错
会话开始结束思考子任务完成压缩上下文工具出错已批准拒绝连接就绪启动等待正在
```

改为：

```text
# 设备自身：已批准 / 已拒绝 / 就绪 / 等待 / 失联 / 恢复 / 请到电脑答题
# hook_client.py 的状态小字：会话开始 / 会话结束 / 思考中 / 子任务完成 /
#                     压缩上下文 / 任务完成 / <工具> 完成 / <工具> 出错
会话开始结束思考子任务完成压缩上下文工具出错已批准拒绝连接就绪启动等待正在电脑答题
```

- [ ] **Step 2: 生成器自检**

```bash
python tools/gen_cn_font.py --verify
```

Expected: 黄金字形自检通过（`中` / `A` 逐位一致），零错误。

- [ ] **Step 3: 重新生成字库**

```bash
python tools/gen_cn_font.py --out firmware/VibePet_UNO/cn_font.h
```

Expected: 输出包含总字数（应为 394 字：95 ASCII + 299 汉字）。

- [ ] **Step 4: 离线测试回归**

```bash
g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o /tmp/proto_test \
  && /tmp/proto_test
```

Expected: `55/55 全部通过`。

- [ ] **Step 5: 编译并记录 Flash 增量**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build firmware/VibePet_UNO
```

Expected: 零警告；Flash 约为 30474 + 92 = **30566 B 左右**（记录实际值）。

---

## Task 2: `hook_client` 交互类工具豁免（TDD）

**Files:**
- Modify: `pc/_smoke_test.py`（在汇总打印之前插入）
- Modify: `pc/hook_client.py:398-431`

**Interfaces:**
- Consumes: 现有 `report_state(status, msg)`、`emit_decision(allow, reason, event)`
- Produces: `INTERACTIVE_TOOLS = {"AskUserQuestion", "ExitPlanMode"}`；
  交互类 `PermissionRequest` 恒为 `allow`，并向 daemon 上报
  `{"type":"state","status":"interactive","msg":"请到电脑答题"}`

- [ ] **Step 1: 先写失败测试**

在 `pc/_smoke_test.py` 的 `check("stdin 为空 -> deny 且不崩溃", ...)` 之后、
`print("=" * 68)` 之前，插入：

```python
# —— 交互类工具：AskUserQuestion / ExitPlanMode ——
# 设备只有两个按钮，无法表达多选项；这类工具交给终端原生交互。
# 期望：hook_client 先上报 interactive 状态，然后直接 allow；
#       daemon 不在时也必须 allow（否则正常选择题会被 deny 掉）。
INTERACTIVE_INPUT = {
    "session_id": "test-session",
    "hook_event_name": "PermissionRequest",
    "tool_name": "AskUserQuestion",
    "tool_input": {"questions": [{"question": "选哪个？", "options": ["A", "B"]}]},
}


def check_interactive(name, daemon_online):
    daemon = FakeDaemon({}, reply=True) if daemon_online else None
    time.sleep(0.35)
    try:
        proc = run_client(hook_input=INTERACTIVE_INPUT)
    finally:
        if daemon:
            daemon.close()
        time.sleep(0.3)

    out = proc.stdout.decode("utf-8")
    problems = []
    lines = [ln for ln in out.splitlines() if ln.strip()]
    if len(lines) != 1:
        problems.append(f"stdout 应为 1 行，实际 {len(lines)} 行: {out!r}")
    else:
        try:
            hso = json.loads(lines[0])["hookSpecificOutput"]
            if hso.get("hookEventName") != "PermissionRequest":
                problems.append(f"hookEventName 错误: {hso.get('hookEventName')!r}")
            if hso["decision"]["behavior"] != "allow":
                problems.append(
                    f"交互工具应 allow，实际 {hso['decision']['behavior']!r}")
        except Exception as exc:
            problems.append(f"stdout 不是预期 Hook JSON ({exc}): {out!r}")

    if daemon_online:
        if daemon.received is None:
            problems.append("daemon 未收到 interactive 状态")
        else:
            try:
                forwarded = json.loads(daemon.received)
                want = {"type": "state", "status": "interactive",
                        "msg": "请到电脑答题"}
                if forwarded != want:
                    problems.append(f"状态上报内容错误: {forwarded!r}")
            except Exception as exc:
                problems.append(
                    f"状态上报不是合法 JSON ({exc}): {daemon.received!r}")

    if proc.returncode != 0:
        problems.append(f"退出码应为 0，实际 {proc.returncode}")

    stderr_text = proc.stderr.decode("utf-8", errors="replace").strip()
    results.append((name, not problems, problems, stderr_text))


check_interactive("AskUserQuestion -> 提醒并 allow（daemon 在线）", True)
check_interactive("AskUserQuestion daemon 未运行 -> 仍 allow", False)
```

- [ ] **Step 2: 运行测试，确认失败**

```bash
python pc/_smoke_test.py
```

Expected: `10/12 通过`（新增的两条失败：第一条状态上报内容不对，第二条得到
`deny` 而不是 `allow`）。

- [ ] **Step 3: 实现豁免**

在 `pc/hook_client.py` 的 `handle_approval_event()` 之前加入常量：

```python
# 交互类工具不占用设备审批：它们需要用户在终端里选择/确认，
# 设备只有两个按钮，无法表达多选项。
INTERACTIVE_TOOLS = {"AskUserQuestion", "ExitPlanMode"}
```

在 `handle_approval_event()` 开头、`debug(...)` 之后加入：

```python
    if tool_name in INTERACTIVE_TOOLS:
        # 先让设备亮起「请到电脑答题」提醒，再放行给终端原生交互。
        # report_state 自带超时与静默失败，daemon 不在也不会拖住这里。
        report_state("interactive", "请到电脑答题")
        log(f"交互类工具 {tool_name} 交给终端原生交互，不占用设备审批")
        emit_decision(True, "VibePet: interactive tool, handled in terminal", event)
        return 0
```

- [ ] **Step 4: 运行测试，确认通过**

```bash
python pc/_smoke_test.py
```

Expected: `12/12 通过`。

- [ ] **Step 5: 回归其余冒烟测试**

```bash
python pc/_smoke_test_daemon.py
python pc/_smoke_test_state.py
python pc/_smoke_test_serial.py
python -m py_compile pc/hook_client.py
```

Expected: `14/14`、`14/14`、`9/9`，语法检查无输出。

---

## Task 3: 固件第七态 + DONE 回退 + 竖屏布局

**Files:**
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino`
  - 顶部注释（屏幕方向）
  - 第 102–151 行常量与布局检查
  - 第 153–166 行颜色
  - 第 170–191 行枚举与全局状态
  - 第 572–579 行边框函数
  - 第 619–684 行渲染
  - 第 686–695 行 `setState`
  - 第 703–770 行动画
  - 第 838–845 行状态分派
  - 第 1010–1030 行看门狗
  - 第 1060–1083 行主循环

**Interfaces:**
- Consumes: `beepStart(uint8_t, uint16_t, uint16_t)`、`printTitle(...)`、`drawBodyText(...)`
- Produces: `enum PetState` 增加 `ST_INTERACTIVE`；`drawInteractiveIcon()`；
  `drawBorder(uint16_t color, bool on)` 取代 `drawNeedsBorder(bool)`

- [ ] **Step 1: 更新屏幕方向注释**

把文件头第 41 行：

```c
 * ━━━━━━━━━━━━━━━ 屏幕长什么样（160×128 横屏）━━━━━━━━━━━━━━━
```

改为：

```c
 * ━━━━━━━━━━━━━━━ 屏幕长什么样（128×160 竖屏）━━━━━━━━━━━━━━━
```

- [ ] **Step 2: 替换布局常量与时序常量**

把第 102–134 行替换为：

```c
// ── 时序（毫秒）──
#define WATCHDOG_TIMEOUT 5000   // 超过这么久没收到任何消息 → 显示「失联」
#define BUTTON_STABLE_MS   35   // 按下电平连续稳定这么久才算一次有效按键
#define ANIM_FRAME_MS      60   // 动画换帧间隔（科技动效更细腻）
#define BLINK_MS          500   // 等待审批时边框闪烁的间隔
#define DONE_FRAME_MS      85   // 对勾逐段出现的间隔
#define ERROR_FRAME_MS    260   // 错误烟雾上升一帧的间隔
#define LOST_FRAME_MS     500   // 失联信号闪烁间隔
#define DONE_HOLD_MS     3000   // DONE 停留 3 秒后自动回 IDLE

// ── 屏幕布局（竖屏画布 128 宽 × 160 高）──
#define SCREEN_W   128
#define SCREEN_H   160
#define TITLE_X     0
#define TITLE_Y     6
#define ICON_CX     64    // 图标区中心
#define ICON_CY     60
#define ICON_HALF   32    // 图标区半边长（清理时按这个范围擦）
#define BODY_TOP   102    // 短正文区域的顶边，与图标留出间隔
#define TITLE_H     16    // 内置 5×7 字体放大两倍后的高度
#define ICON_AREA_LEFT   (ICON_CX - ICON_HALF)
#define ICON_AREA_RIGHT  (ICON_CX + ICON_HALF)  // 右边界不包含
#define ICON_AREA_TOP    28
#define ICON_AREA_BOTTOM 94                    // 下边界不包含
#define LOST_SIGNAL_TOP  44
#define LOST_SIGNAL_H    20
#define LOST_TEXT_TOP    76
#define BODY_MARGIN  8   // 左右与底边留白，正文不会贴着审批边框
#define BODY_WIDTH (SCREEN_W - 2 * BODY_MARGIN)
#define BODY_LINE_H 18   // 正文行高（字高 13 + 5 行距）
#define BODY_LINES   6   // 长正文最多六行
#define LONG_BODY_TOP 30 // 长正文让出标题栏后直接开始
#define HEADER_DOT_X 14
#define HEADER_DOT_Y 12
```

- [ ] **Step 3: 增加交互态颜色**

在颜色区第 163 行 `#define C_LOST ST77XX_ORANGE` 之后加入：

```c
#define C_INTERACTIVE 0x6C1F   // 蓝紫：交互提醒（CHECK PC）
```

- [ ] **Step 4: 增加第七态与状态计时**

把枚举行：

```c
enum PetState { ST_IDLE, ST_WORKING, ST_NEEDS_YOU, ST_DONE, ST_ERROR, ST_LOST };
```

改为：

```c
enum PetState { ST_IDLE, ST_WORKING, ST_NEEDS_YOU, ST_DONE, ST_ERROR, ST_LOST,
                ST_INTERACTIVE };
```

在 `static unsigned long lastBlinkMs = 0;` 之后加入：

```c
static unsigned long stateSince = 0;        // 进入当前状态的时刻（DONE 回退用）
```

- [ ] **Step 5: `setState` 记录进入时刻**

在 `setState()` 中 `currentState = state;` 之后加入：

```c
  stateSince = millis();
```

- [ ] **Step 6: 边框函数参数化**

把 `drawNeedsBorder`：

```c
static void drawNeedsBorder(bool on) {
  uint16_t color = on ? C_NEEDS : C_BG;
  tft.fillRect(0, 0, SCREEN_W, 2, color);
  tft.fillRect(0, SCREEN_H - 2, SCREEN_W, 2, color);
  tft.fillRect(0, 0, 2, SCREEN_H, color);
  tft.fillRect(SCREEN_W - 2, 0, 2, SCREEN_H, color);
  pumpSerial();
}
```

改为：

```c
// 四条边一起闪：on=false 时用背景色擦掉。颜色由调用方给，
// 审批用黄色、交互提醒用蓝紫。
static void drawBorder(uint16_t color, bool on) {
  uint16_t c = on ? color : C_BG;
  tft.fillRect(0, 0, SCREEN_W, 2, c);
  tft.fillRect(0, SCREEN_H - 2, SCREEN_W, 2, c);
  tft.fillRect(0, 0, 2, SCREEN_H, c);
  tft.fillRect(SCREEN_W - 2, 0, 2, SCREEN_H, c);
  pumpSerial();
}
```

把 `renderState()` 与 `updateAnimation()` 里的 `drawNeedsBorder(...)` 全部替换为
`drawBorder(C_NEEDS, ...)`。

- [ ] **Step 7: 新增交互图标的基础版**

在 `drawNeedsIcon()` 之后加入：

```c
// 交互提醒：电脑显示器轮廓 + 从设备指向屏幕的箭头。
// 选择题/计划确认只能在电脑上操作，设备只负责喊你过去。
static void drawInteractiveIcon() {
  tft.drawRoundRect(ICON_CX - 22, ICON_CY - 22, 44, 30, 3, C_INTERACTIVE);
  tft.drawFastHLine(ICON_CX - 8, ICON_CY + 8, 16, C_INTERACTIVE);
  tft.drawFastVLine(ICON_CX, ICON_CY + 8, 4, C_INTERACTIVE);
  if (animPhase & 1) {
    tft.fillRect(ICON_CX - 14, ICON_CY - 12, 7, 2, C_INTERACTIVE);
  }
  int16_t ay = ICON_CY + 24 + (int16_t)(animPhase & 3) * 3;
  tft.drawLine(ICON_CX, ay, ICON_CX, ay - 6, C_INTERACTIVE);
  tft.drawLine(ICON_CX, ay - 6, ICON_CX - 3, ay - 3, C_INTERACTIVE);
  tft.drawLine(ICON_CX, ay - 6, ICON_CX + 3, ay - 3, C_INTERACTIVE);
  pumpSerial();
}
```

- [ ] **Step 8: 状态颜色与渲染**

在 `stateColor()` 中加入：

```c
    case ST_INTERACTIVE: return C_INTERACTIVE;
```

在 `renderState()` 的标题 switch 中加入：

```c
    case ST_INTERACTIVE: printTitle(F("CHECK PC"), 8, color); break;
```

在 `renderState()` 的图标 switch 中加入：

```c
    case ST_INTERACTIVE: borderOn = true; drawInteractiveIcon(); drawBorder(C_INTERACTIVE, true); break;
```

- [ ] **Step 9: 状态分派**

在 `dispatchLine()` 的 `state` 分支、`error` 判断之后加入：

```c
    else if (strcmp(statusBuf, "interactive") == 0) {
      beepStart(2, 35, 30);      // 两声短鸣：去电脑上答题
      setState(ST_INTERACTIVE);  // 最后才重绘（铁律）
    }
```

- [ ] **Step 10: 看门狗恢复与 LED**

把 LOST 恢复逻辑改为：

```c
    PetState back = (lastValidState == ST_NEEDS_YOU ||
                     lastValidState == ST_INTERACTIVE)
                        ? ST_IDLE : lastValidState;
    setState(back);
```

把 LED 判定改为：

```c
  bool approvalPending = (ridBuf[0] != '\0');
  bool needAttention = approvalPending || currentState == ST_INTERACTIVE;
  digitalWrite(PIN_LED_STATUS, (isLost || needAttention) ? HIGH : LOW);
```

- [ ] **Step 11: 主循环加入 DONE 回退**

在 `loop()` 的 `checkWatchdog();` 之后加入：

```c
  // DONE 只停留 3 秒：任务完成后自动回空闲，避免屏幕一直停在 DONE。
  // 期间如果收到新状态，setState 会刷新 stateSince，这条自然不再触发。
  if (currentState == ST_DONE && millis() - stateSince >= DONE_HOLD_MS) {
    msgBuf[0] = '\0';      // 清掉「任务完成」等正文，避免 IDLE 挂着旧字
    setState(ST_IDLE);
  }
```

- [ ] **Step 12: 动画分支**

在 `updateAnimation()` 的 switch 中加入：

```c
    case ST_INTERACTIVE:
      if (now - lastAnimMs >= ANIM_FRAME_MS) {
        lastAnimMs = now;
        animPhase++;
        clearIconArea();
        drawInteractiveIcon();
        if (now - lastBlinkMs >= BLINK_MS) {
          lastBlinkMs = now;
          borderOn = !borderOn;
          drawBorder(C_INTERACTIVE, borderOn);
        }
      }
      break;
```

- [ ] **Step 13: 编译并回归**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build firmware/VibePet_UNO
g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o /tmp/proto_test \
  && /tmp/proto_test
```

Expected: 零警告；Flash ≤ 32256；`55/55 全部通过`。

---

## Task 4: 科技精致动画

**Files:**
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino`（绘制区与动画区）

**Interfaces:**
- Consumes: Task 3 的 `drawBorder(uint16_t, bool)`、`ST_INTERACTIVE`
- Produces: `blend565(uint16_t, uint16_t, uint8_t)`、`lighten565(uint16_t)`、
  `drawHalo(int16_t, int16_t, uint8_t, uint16_t)`、`drawSweep(int16_t, uint16_t)`、
  `drawStars(uint8_t, uint16_t)`

- [ ] **Step 1: 加入共用绘制工具**

在 `thickLine()` 之后加入：

```c
// 颜色插值：t=0 返回 a，t=255 返回 b。整数运算，不用浮点。
// 除以 256 用右移近似，视觉上看不出差别，还省下软件除法的代码量。
static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) {
  int16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int16_t r = ar + (((br - ar) * (int16_t)t) >> 8);
  int16_t g = ag + (((bg - ag) * (int16_t)t) >> 8);
  int16_t bl = ab + (((bb - ab) * (int16_t)t) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// 每个通道提亮一点，用来画高光。
static uint16_t lighten565(uint16_t c) {
  int16_t r = ((c >> 11) & 0x1F) + 6; if (r > 31) r = 31;
  int16_t g = ((c >> 5) & 0x3F) + 12; if (g > 63) g = 63;
  int16_t b = (c & 0x1F) + 6; if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// 两层同心圆光晕：亮的一圈 + 暗的一圈。
static void drawHalo(int16_t cx, int16_t cy, uint8_t r, uint16_t color) {
  tft.drawCircle(cx, cy, r, color);
  tft.drawCircle(cx, cy, r + 2, DIM(color));
  pumpSerial();
}

// 图标区里的一条横向扫描线。
static void drawSweep(int16_t y, uint16_t color) {
  tft.drawFastHLine(ICON_AREA_LEFT, y, ICON_AREA_RIGHT - ICON_AREA_LEFT, color);
  pumpSerial();
}

// 8 个固定星点，按相位轮流亮起。坐标放 Flash，不占内存。
static const int8_t STAR_X[8] PROGMEM = { -26, 22, -14, 28, 0, -30, 12, 30 };
static const int8_t STAR_Y[8] PROGMEM = { -18, -24, 20, 8, -26, 6, 28, -6 };

static void drawStars(uint8_t phase, uint16_t color) {
  for (uint8_t i = 0; i < 8; i++) {
    if (((phase + i) & 3) == 0) {
      tft.drawPixel(ICON_CX + (int8_t)pgm_read_byte(&STAR_X[i]),
                    ICON_CY + (int8_t)pgm_read_byte(&STAR_Y[i]), color);
    }
  }
  pumpSerial();
}
```

- [ ] **Step 2: 增强 IDLE**

把 `drawIdleIcon()` 替换为：

```c
static void drawIdleIcon() {
  static uint8_t lastRadius = 0;
  uint8_t phase = animPhase & 0x0F;
  uint8_t step = (phase < 8) ? phase : (uint8_t)(15 - phase);
  uint8_t radius = (uint8_t)(11 + step / 2);

  if (lastRadius > 0) tft.fillCircle(ICON_CX, ICON_CY, lastRadius + 6, C_BG);
  drawHalo(ICON_CX, ICON_CY, radius + 3,
           blend565(C_IDLE, C_BG, 180));
  tft.fillCircle(ICON_CX, ICON_CY, radius, C_IDLE);
  tft.fillCircle(ICON_CX - 3, ICON_CY - 3, 2, lighten565(C_IDLE));

  tft.fillRect(ICON_CX - 4, ICON_CY - 2, 2, 3, C_BG);
  tft.fillRect(ICON_CX + 3, ICON_CY - 2, 2, 3, C_BG);
  tft.drawPixel(ICON_CX - 2, ICON_CY + 4, C_BG);
  tft.drawFastHLine(ICON_CX - 1, ICON_CY + 5, 3, C_BG);
  tft.drawPixel(ICON_CX + 2, ICON_CY + 4, C_BG);
  if (phase == 7 || phase == 8) {
    tft.drawFastHLine(ICON_CX - 5, ICON_CY - 1, 4, C_BG);
    tft.drawFastHLine(ICON_CX + 2, ICON_CY - 1, 4, C_BG);
  }

  drawStars(animPhase, DIM(C_IDLE));
  drawSweep(ICON_AREA_TOP + ((animPhase * 5) %
                             (ICON_AREA_BOTTOM - ICON_AREA_TOP)),
            blend565(C_IDLE, C_BG, 230));
  pumpSerial();
  lastRadius = radius;
}
```

- [ ] **Step 3: 增强 WORKING**

把 `drawWorkingIcon()` 替换为：

```c
#define WORK_DOTS 5
static void drawWorkingIcon() {
  int16_t x0 = ICON_CX - (WORK_DOTS * 10 - 2) / 2;
  uint8_t lit = (uint8_t)(animPhase % WORK_DOTS);
  uint16_t bright = lighten565(C_WORKING);

  // 两条暗色竖网格
  tft.drawFastVLine(ICON_CX - 26, ICON_AREA_TOP + 4, 44, blend565(C_WORKING, C_BG, 220));
  tft.drawFastVLine(ICON_CX + 26, ICON_AREA_TOP + 4, 44, blend565(C_WORKING, C_BG, 220));

  for (uint8_t i = 0; i < WORK_DOTS; i++) {
    uint16_t c = (i == lit) ? C_WORKING : DIM(C_WORKING);
    tft.fillRect(x0 + i * 10, ICON_CY - 4, 8, 8, c);
    if (i == lit) tft.drawFastHLine(x0 + i * 10, ICON_CY - 4, 8, bright);
  }
  tft.drawRoundRect(ICON_CX - 22, ICON_CY + 9, 44, 12, 3, DIM(C_WORKING));
  for (uint8_t i = 0; i < WORK_DOTS; i++) {
    uint16_t keyColor = (i == (lit / 2)) ? bright : DIM(C_WORKING);
    tft.fillRect(ICON_CX - 18 + i * 7, ICON_CY + 12, 4, 3, keyColor);
  }
  tft.fillRect(ICON_CX - 5, ICON_CY + 17, 10, 1, DIM(C_WORKING));
  drawSweep(ICON_AREA_TOP + ((animPhase * 7) %
                             (ICON_AREA_BOTTOM - ICON_AREA_TOP)),
            blend565(C_WORKING, C_BG, 225));
  pumpSerial();
}
```

- [ ] **Step 4: 增强 NEEDS**

把 `drawNeedsIcon(bool raysOn)` 替换为：

```c
static void drawNeedsIcon(bool raysOn) {
  uint8_t stretch = (uint8_t)((animPhase & 3) + 1);
  drawHalo(ICON_CX, ICON_CY - 2, 18, blend565(C_NEEDS, C_BG, 170));
  tft.fillRect(ICON_CX - 3, ICON_CY - 14, 6, 20, C_NEEDS);
  tft.fillRect(ICON_CX - 2, ICON_CY - 13, 2, 16, lighten565(C_NEEDS));
  tft.fillCircle(ICON_CX, ICON_CY + 13, 3, C_NEEDS);

  uint16_t rayColor = raysOn ? C_NEEDS : blend565(C_NEEDS, C_BG, 180);
  tft.drawLine(ICON_CX - 10, ICON_CY - 9,
               ICON_CX - 10 - stretch * 2, ICON_CY - 9 - stretch * 2, rayColor);
  tft.drawLine(ICON_CX + 10, ICON_CY - 9,
               ICON_CX + 10 + stretch * 2, ICON_CY - 9 - stretch * 2, rayColor);
  drawStars(animPhase, blend565(C_NEEDS, C_BG, 200));
  pumpSerial();
}
```

`updateAnimation()` 的 `ST_NEEDS_YOU` 分支把 `drawNeedsBorder(borderOn)` 改为
`drawBorder(borderOn ? lighten565(C_NEEDS) : C_NEEDS, true)`；`false` 时仍要擦边框，
所以写成：

```c
        drawBorder(borderOn ? lighten565(C_NEEDS) : C_NEEDS, borderOn);
```

- [ ] **Step 5: 增强 DONE**

把 `drawDoneIcon(uint8_t frame)` 替换为：

```c
static void drawDoneIcon(uint8_t frame) {
  if (frame == 0) {
    tft.fillCircle(ICON_CX - 12, ICON_CY, 2, C_DONE);
    return;
  }

  if (frame == 1) {
    thickLine(ICON_CX - 12, ICON_CY, ICON_CX - 7, ICON_CY + 5, 3, C_DONE);
    pumpSerial();
    return;
  }

  thickLine(ICON_CX - 12, ICON_CY, ICON_CX - 3, ICON_CY + 9, 3, C_DONE);
  thickLine(ICON_CX - 4, ICON_CY + 8, ICON_CX + 12, ICON_CY - 9, 3, C_DONE);
  tft.drawPixel(ICON_CX - 10, ICON_CY + 1, lighten565(C_DONE));

  if (frame < 4) {
    pumpSerial();
    return;
  }

  // 完成后向外扩散的光环 + 星点
  uint8_t ring = (uint8_t)(((animPhase - 4) & 3) * 4 + 16);
  tft.drawCircle(ICON_CX, ICON_CY, ring, blend565(C_DONE, C_BG, 140));
  drawStars(animPhase, C_DONE);
  pumpSerial();
}
```

`updateAnimation()` 的 `ST_DONE` 分支改为持续刷新（不再停在 4）：

```c
    case ST_DONE:
      if (now - lastAnimMs >= DONE_FRAME_MS) {
        lastAnimMs = now;
        animPhase++;
        clearIconArea();
        drawDoneIcon(animPhase);
      }
      break;
```

`DONE_HOLD_MS` 会在 3 秒时把状态切走，不会无限播放。

- [ ] **Step 6: 增强 ERROR**

把 `drawErrorIcon(uint8_t frame)` 替换为：

```c
static void drawErrorIcon(uint8_t frame) {
  drawHalo(ICON_CX, ICON_CY, 16, blend565(C_ERROR, C_BG, 150));
  thickLine(ICON_CX - 10, ICON_CY - 10, ICON_CX + 10, ICON_CY + 10, 3, C_ERROR);
  thickLine(ICON_CX + 10, ICON_CY - 10, ICON_CX - 10, ICON_CY + 10, 3, C_ERROR);
  tft.drawPixel(ICON_CX - 8, ICON_CY - 8, lighten565(C_ERROR));

  if (frame != 0) {
    int8_t lift = (int8_t)((frame & 1) ? 0 : 1);
    tft.fillCircle(ICON_CX + 18, ICON_CY - 10 - lift, 4, blend565(C_ERROR, C_BG, 180));
    tft.fillCircle(ICON_CX + 23, ICON_CY - 15 - lift, 3, blend565(C_ERROR, C_BG, 120));
    tft.fillCircle(ICON_CX + 15, ICON_CY - 18 - lift, 2, C_ERROR);
  }
  drawSweep(ICON_AREA_TOP + ((animPhase * 9) %
                             (ICON_AREA_BOTTOM - ICON_AREA_TOP)),
            blend565(C_ERROR, C_BG, 225));
  pumpSerial();
}
```

- [ ] **Step 7: 增强 LOST**

把 `drawLostSignal(bool wavesOn)` 替换为：

```c
static void drawLostSignal(bool wavesOn) {
  const int16_t x = ICON_CX - 18;
  const int16_t y = LOST_SIGNAL_TOP;
  tft.fillRect(x, y, 36, 8, C_LOST); pumpSerial();
  tft.fillRect(x, y + 8, 36, 8, C_LOST); pumpSerial();
  tft.fillRect(x, y + 16, 36, LOST_SIGNAL_H - 16, C_LOST); pumpSerial();

  if (wavesOn) {
    tft.drawLine(ICON_CX - 12, y + 11, ICON_CX, y + 2, C_BG);
    tft.drawLine(ICON_CX, y + 2, ICON_CX + 12, y + 11, C_BG);
    tft.drawLine(ICON_CX - 7, y + 12, ICON_CX, y + 7, C_BG);
    tft.drawLine(ICON_CX, y + 7, ICON_CX + 7, y + 12, C_BG);
    tft.fillCircle(ICON_CX, y + 16, 2, C_BG);
    tft.drawCircle(ICON_CX, y + 2, 16, DIM(C_LOST));
  }
  tft.drawLine(ICON_CX - 8, y + 17, ICON_CX + 8, y + 1, C_BG);

  // 大字上方一条横向扫描线
  int16_t sy = LOST_TEXT_TOP - 8 + (int16_t)(animPhase & 3) * 2;
  tft.drawFastHLine(24, sy, SCREEN_W - 48, DIM(C_LOST));
  pumpSerial();
}
```

- [ ] **Step 8: 增强 INTERACTIVE**

把 Task 3 的 `drawInteractiveIcon()` 替换为：

```c
static void drawInteractiveIcon() {
  uint16_t lit = C_INTERACTIVE;
  uint16_t dim = blend565(C_INTERACTIVE, C_BG, 170);

  // 显示器外框 + 底座
  tft.drawRoundRect(ICON_CX - 22, ICON_CY - 22, 44, 30, 3, lit);
  tft.drawFastHLine(ICON_CX - 8, ICON_CY + 8, 16, lit);
  tft.drawFastVLine(ICON_CX, ICON_CY + 8, 4, lit);

  // 屏幕内部的扫描线与闪烁光标
  int16_t inY = ICON_CY - 19 + (int16_t)((animPhase * 3) % 24);
  tft.drawFastHLine(ICON_CX - 17, inY, 34, dim);
  if (animPhase & 1) {
    tft.fillRect(ICON_CX - 14, ICON_CY - 12, 7, 2, lit);
  }

  // 从下往上循环的箭头
  int16_t ay = ICON_CY + 24 + (int16_t)(animPhase & 3) * 3;
  tft.drawLine(ICON_CX, ay, ICON_CX, ay - 6, lit);
  tft.drawLine(ICON_CX, ay - 6, ICON_CX - 3, ay - 3, lit);
  tft.drawLine(ICON_CX, ay - 6, ICON_CX + 3, ay - 3, lit);

  drawStars(animPhase, dim);
  pumpSerial();
}
```

`updateAnimation()` 的 `ST_INTERACTIVE` 分支边框改为：

```c
          drawBorder(borderOn ? lighten565(C_INTERACTIVE) : C_INTERACTIVE,
                     borderOn);
```

- [ ] **Step 9: 编译与预算检查**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build firmware/VibePet_UNO
```

Expected: 零警告；Flash ≤ 32256。

**若超限**，按顺序裁剪：① 去掉 `drawStars` 调用（保留函数或删除）；② 去掉
`drawSweep` 调用；③ 去掉 `drawHalo` 的第二层圆；④ 删掉 ERROR/LOST 的扫描线。
每次裁剪后重新编译。

- [ ] **Step 10: 离线回归**

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

## Task 5: 文档同步

**Files:**
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: `.claude/README.md`
- Modify: `VibePet —— AI 编程助手物理状态显示与审批终端（USB 有线版）v3.0.md`

**Interfaces:**
- Consumes: Task 1–4 的最终行为
- Produces: 文档与实现一致

- [ ] **Step 1: 更新 `CLAUDE.md`**

1. 状态表增加一行：

```markdown
| `interactive` | 蓝紫 | 显示器动画 + "CHECK PC" + "请到电脑答题" |
```

2. 把「当前状态」里 `2026-10-24` 的记录扩展为：竖屏 128×160、DONE 3 秒回 IDLE、
   七态、科技动画、交互工具豁免与 CHECK PC 提醒；写入实际 Flash/RAM 数字。
3. 硬件接线表里的「状态 LED」说明改为「失联 / 待审批 / 交互提醒时点亮」。
4. 五、Hook 事件表增加：`PermissionRequest` 且工具为 `AskUserQuestion` /
   `ExitPlanMode` → 上报 `interactive` → 直接 allow。

- [ ] **Step 2: 更新 `README.md`**

1. 状态表增加 `CHECK PC` 行：蓝紫、显示器动画、「请到电脑答题」。
2. 「日常使用 → 审批」补充：选择题/计划确认不会占用设备审批，设备只亮提醒，
   回答仍在电脑上完成。
3. 屏幕描述从横屏改为竖屏 128×160（如有提及）。
4. 排障表补充：设备显示 CHECK PC 但电脑没有弹窗 → 检查 daemon 是否在运行。

- [ ] **Step 3: 更新 `.claude/README.md`**

把「交互类工具待确认」那条改为已实现：

```markdown
| 交互类工具 | ✅ `AskUserQuestion` / `ExitPlanMode` 不占用设备审批：`hook_client` 上报
`interactive` 后直接 allow，设备显示 `CHECK PC`；daemon 离线也照样放行 |
```

- [ ] **Step 4: 更新设计文档 v3.0**

1. 六态表增加 `interactive` 一行（蓝紫）。
2. 屏幕方向与分辨率从 160×128 横屏改为 128×160 竖屏。
3. 硬件接线不变（引脚没有改动）。

---

## Task 6: 上机联调

**Files:**
- Modify: `CLAUDE.md`（记录实测结果）
- Modify: `.claude/settings.json`（仅在用户明确授权时启用/禁用）

**Interfaces:**
- Consumes: Task 1–5 的最终固件与 hook 配置
- Produces: 真机验收结果

> ⚠️ **前置条件**：当前项目级 9 个 hook 处于启用状态，而 daemon 已经停止。
> 此时任何 Bash 调用都会被 `PreToolUse` 拒绝。开始前必须二选一：
> ① 用户手动运行 `python pc/bridge_daemon.py`；或
> ② 用户明确授权把 hook 临时改回注释。
>
> ⚠️ **烧录前必须停 daemon**（串口被占用）。

- [ ] **Step 1: 确认端口与进程**

```bash
"/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe" board list
```

Expected: `COM9` 在列表中；烧录前无 daemon 占用串口。

- [ ] **Step 2: 编译并烧录**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build \
  --upload -p COM9 firmware/VibePet_UNO
```

Expected: 上传成功。

- [ ] **Step 3: 启动 daemon**

```bash
python pc/bridge_daemon.py
```

后台运行，确认日志出现「设备已启动（固件 uno/1.0），补发当前画面」。

- [ ] **Step 4: 竖屏方向**

观察屏幕：标题在上、图标居中、正文在下。若画面倒置，把
`tft.setRotation(0)` 改为 `tft.setRotation(2)` 后重新编译烧录。

- [ ] **Step 5: 六态 + 交互态动画**

用 `tools/serial_probe.py COM9`（先停 daemon）逐个发送：

```text
/state idle
/state working 正在分析代码
/approval 删除缓存
/state done 任务完成
/state error 出错了
/state interactive 请到电脑答题
```

Expected: 竖屏布局不越界；渐变、光晕、扫描线可见；`CHECK PC` 标题与正文正确。

- [ ] **Step 6: DONE 回退**

发送 `/state done 任务完成` 后不再发任何消息（探针需加 `--no-heartbeat` 防止心跳保活）：

```bash
python tools/serial_probe.py COM9 --no-heartbeat
```

Expected: `DONE` 动画播放约 3 秒后自动回到 `IDLE`，正文清空，LED 熄灭。

- [ ] **Step 7: 交互提醒（真实 Claude Code）**

启动 daemon 并确保 hook 启用。让 Claude Code 触发一次 `AskUserQuestion`
（例如让助手问一个选择题）：

Expected: 设备显示 `CHECK PC` + 「请到电脑答题」，LED 亮，两声短鸣；
终端原生选择题正常出现；答完后画面回到 `WORKING`。

- [ ] **Step 8: 审批回归**

- Bash 批准 → 命令放行；
- Bash 拒绝 → `Denied by VibePet`；
- 一次 `PermissionRequest`（如读取用户级配置）→ 设备批准后放行。

Expected: 与改造前一致。

- [ ] **Step 9: 失联与恢复**

停 daemon，等 5 秒：屏幕 `LOST`、LED 亮；重启 daemon：恢复正常、LED 灭；
若失联前是 `CHECK PC`，恢复后显示 `IDLE`（不残留提醒）。

- [ ] **Step 10: 长中文摘要**

触发一条长中文 Bash 命令的审批，确认竖屏每行约 9 个汉字、6 行内折行不越界。

- [ ] **Step 11: 记录结果并清理**

把实测结果写入 `CLAUDE.md` 的 2026-10-24 记录；确认 `git status --short` 无临时文件。

---

## Self-Review

- **Spec coverage:** DONE 3 秒 → Task 3 Step 11；竖屏 → Task 3 Step 2；科技动画 → Task 4；
  交互工具豁免与 CHECK PC → Task 2 + Task 3 Step 7–9；字库 → Task 1；文档 → Task 5；
  上机 → Task 6。
- **Placeholder scan:** 无 TBD/TODO；所有代码改动均给出完整替换内容。
- **Type consistency:** `drawBorder(uint16_t, bool)` 在 Task 3 定义、Task 4 使用；
  `blend565` / `lighten565` / `drawHalo` / `drawSweep` / `drawStars` 在 Task 4 Step 1
  定义、后续步骤使用；`ST_INTERACTIVE` 在 Task 3 定义。
- **预算风险:** Task 4 Step 9 明确给出裁剪顺序。
