# 竖屏 + 科技动画 + 交互提醒 + DONE 回退设计

日期：2026-10-24
状态：已与用户确认

## 背景

当前固件是横屏 160×128，`Stop` 事件把屏幕切到 `DONE` 后没有任何回退逻辑，会一直停到
下一次会话事件；`PermissionRequest` hook 没有 matcher，`AskUserQuestion` / `ExitPlanMode`
这类交互工具也会进入设备审批，但设备只有两个按钮，无法表达多选项，daemon 离线时还会
把它们直接 `deny` 掉。用户要求：竖屏、DONE 自动回 `IDLE`、交互工具改为「请到电脑答题」
提醒动画，并整体提升动画精细度与色彩丰富度。

## 需求（用户已确认）

1. `DONE` 停留 **3 秒**后自动回到 `IDLE`；期间收到新状态立即以新状态为准。
2. 屏幕改为**竖屏 128×160**。
3. 六态动画升级为**科技精致**风格：渐变、光晕、扫描线、粒子，整数运算，不引入浮点。
4. 交互类工具（`AskUserQuestion` / `ExitPlanMode`）不占用设备审批：
   - `hook_client.py` 先上报状态 `interactive`，再直接输出 `allow`；
   - 设备显示标题 `CHECK PC`、正文「请到电脑答题」与提醒动画；
   - LED 点亮、蜂鸣器 2 声短鸣；持续到下一个状态事件。

## 设计

### 1. 交互类工具：新增第七态 `interactive`

**电脑端（`pc/hook_client.py`）**

```python
INTERACTIVE_TOOLS = {"AskUserQuestion", "ExitPlanMode"}

def handle_approval_event(event, hook_input):
    tool_name = hook_input.get("tool_name", "")
    if tool_name in INTERACTIVE_TOOLS:
        # 设备只做提醒，不参与决策：终端会弹原生选择题/计划确认。
        report_state("interactive", "请到电脑答题")
        emit_decision(True, "VibePet: interactive tool, handled in terminal", event)
        return 0
    ...
```

- `report_state()` 是现有的状态上报函数（1 秒超时、失败静默）。
- 即使 daemon 不在，也照常输出 `allow`，不再把正常交互 `deny` 掉。

**daemon（`pc/bridge_daemon.py`）**：`send_state()` 已透传任意状态字符串，无需改动。

**设备端**

- `PetState` 增加 `ST_INTERACTIVE`，`stateColor()` 返回蓝紫色 `0x6C1F`
  （约 RGB 104,128,248，与 `DONE` 的青色区分）。
- 标题 `CHECK PC`（7 字符 × 12 = 84 px，居中）；正文「请到电脑答题」。
- 图标动画：显示器轮廓（矩形 + 底座）+ 屏幕内闪烁光标 + 从下方指向屏幕的箭头/波纹
  循环；配合蓝紫扫描线与边框脉冲。
- 进入状态时 `beepStart(2, 35, 30)`；LED 判定改为
  `isLost || approvalPending || currentState == ST_INTERACTIVE`。
- 状态持续到下一个 `state` 事件（用户答完题后 `PostToolUse` 会发 `working`）。
- LOST 恢复时把 `ST_INTERACTIVE` 也视为过期：`back = (lastValidState == ST_NEEDS_YOU || lastValidState == ST_INTERACTIVE) ? ST_IDLE : lastValidState;`

### 2. DONE → IDLE

- 新增 `#define DONE_HOLD_MS 3000`。
- `setState()` 统一记录 `stateSince = millis()`（所有状态切换）。
- 主循环中：

```cpp
if (currentState == ST_DONE && millis() - stateSince >= DONE_HOLD_MS) {
  msgBuf[0] = '\0';      // 清掉「任务完成」等正文，避免 IDLE 还挂着旧字
  setState(ST_IDLE);
}
```

- 期间收到任何新 `state` 都会走 `setState()`，计时自然重置。
- `lastValidState` 随 `setState(ST_IDLE)` 变为 `IDLE`，LOST 恢复语义不受影响。

### 3. 竖屏布局（128×160）

`tft.setRotation(0)`；若烧录后画面倒置，改 `setRotation(2)`（只改一个常量）。

| 常量 | 旧值（横屏） | 新值（竖屏） |
|---|---|---|
| `SCREEN_W` / `SCREEN_H` | 160 / 128 | **128 / 160** |
| `TITLE_Y` / `TITLE_H` | 4 / 16 | **6 / 16** |
| `ICON_CX` / `ICON_CY` | 80 / 40 | **64 / 60** |
| `ICON_HALF` | 24 | **32** |
| `ICON_AREA_LEFT/RIGHT` | 56 / 104 | **32 / 96**（由 `ICON_CX ± ICON_HALF` 得出） |
| `ICON_AREA_TOP/BOTTOM` | 20 / 68 | **28 / 94** |
| `BODY_TOP` | 70 | **102** |
| `LONG_BODY_TOP` | 34 | **30** |
| `BODY_LINES` | 5 | **6** |
| `LOST_SIGNAL_TOP` / `LOST_TEXT_TOP` | 15 / 44 | **44 / 76** |
| `BODY_MARGIN` / `BODY_WIDTH` | 8 / 144 | **8 / 112（每行约 9 个汉字）** |

编译期检查保持并通过：
- 图标区顶 28 ≥ 标题底 22
- 图标区底 94 ≤ 正文顶 102
- LOST 信号底 64 ≤ LOST 文字顶 76
- 长正文 6 行：30 + 5×18 + 13 = 133 ≤ 152（底边留白）
- 短正文 2 行：102 + 18 + 13 = 133 ≤ 152

### 4. 科技精致动画

**共用工具（新增，尽量省 Flash）**

- `uint16_t blend565(uint16_t a, uint16_t b, uint8_t t)`：整数线性插值，用于渐变。
- `LIGHT(c)` 宏：每通道提亮一档（宏不占 Flash）。
- `drawHalo(cx, cy, r, color)`：两层同心圆光晕。
- `drawSweep(y, color)`：整宽扫描线。
- 粒子坐标表放 **PROGMEM**，8 个固定点，按相位显示 2–3 个。

**各状态效果**

| 状态 | 增强内容 |
|---|---|
| IDLE | 呼吸圆外两层暗色光晕、3 个环绕星点、眨眼保留、扫描线每 8 帧扫过图标区 |
| WORKING | 光标块顶边高光、键盘按键相位闪烁、两条暗色竖网格、扫描线循环 |
| NEEDS_YOU | 感叹号光晕、光芒长度随相位伸缩、边框在主色/亮色间交替、扫描线 |
| DONE | 对勾逐段绘制后向外扩散两层光环 + 4 个星点闪烁 |
| ERROR | 红叉暗红光晕、3 个烟团上下浮动、扫描线自上而下 |
| LOST | 波纹动画、大字上方横向扫描、边框橙/暗橙脉冲 |
| INTERACTIVE | 显示器轮廓 + 闪烁光标 + 循环箭头/波纹、蓝紫扫描线与边框脉冲 |

- 动画帧间隔 `ANIM_FRAME_MS` 90 → **60 ms**；每帧仍分片绘制并 `pumpSerial()`，
  保证任一连续阻塞 ≤ 3 ms。
- 不新增浮点、不新增 `delay()`、不使用 `String`/动态内存。

### 5. 字库

当前 `tools/cn_charset.txt` **缺「电、脑、答、题」**（「请」「到」已有）。
把这 4 个字加入清单，重新生成 `cn_font.h`；每个字约 23 字节，Flash 增加约 92 字节。

命令：

```bash
python tools/gen_cn_font.py --verify
python tools/gen_cn_font.py --out firmware/VibePet_UNO/cn_font.h
```

### 6. 协议与状态表更新

新增第七态：

| 状态值 | 颜色 | 屏幕效果 |
|---|---|---|
| `interactive` | 蓝紫 `0x6C1F` | 显示器图标 + `CHECK PC` + 「请到电脑答题」 |

需要同步更新：`CLAUDE.md` 的状态映射表、`README.md` 的状态表、设计文档 v3.0 的六态表
（改为七态，或在旁边注明交互提醒态）。

## 资源预算

- 当前 Flash 30474 / 32256（余 **1782 B**），RAM 1287 / 2048。
- 预计新增：动画效果 800–1200 B、字库 92 B、状态/计时逻辑 100–200 B，合计
  **约 1000–1500 B**，接近上限。
- 若编译后超限，按以下顺序裁剪：① 去掉粒子与星点；② 去掉状态过渡帧；
  ③ 精简 ERROR/LOST 的扫描线；④ 合并重复绘制代码。**不得削减协议与审批安全路径**。

## 测试与验收

### 离线

- `g++` 离线测试 55/55 通过。
- 四套 Python 冒烟测试：10 / 14 / 14 / 9 全过。
- 新增 `hook_client` 用例：`AskUserQuestion` 在 daemon 不在时也输出 `allow`，
  且尝试上报 `interactive`。
- 固件编译零警告；记录 Flash/RAM。

### 上机（COM9）

1. 竖屏方向正确（倒置则改 `setRotation(2)`）。
2. 六态 + `interactive` 动画平滑、颜色层次明显、无越界。
3. `DONE` 3 秒后自动回 `IDLE`，正文清空。
4. 触发 `AskUserQuestion`（或 `ExitPlanMode`）：设备显示 `CHECK PC` + 「请到电脑答题」，
   LED 亮、两声短鸣；终端原生选择题正常出现；答完后画面回到 `WORKING`。
5. 审批回归：Bash 批准 / 拒绝、`PermissionRequest` 批准仍正常。
6. 失联：LED 亮、`LOST`；恢复后正常，`interactive` 不会残留。
7. 长中文摘要：竖屏 9 字/行折行正常。

## 风险

- **Flash 余量紧张**：动画增强可能超限，按上面的裁剪顺序处理。
- **3 ms 阻塞纪律**：新增绘制必须分片；每帧增量不能超过约 8 行像素。
- **交互工具与 Clawd 并行 hook**：`AllowInteractive` 后 Clawd 的 `PermissionRequest`
  HTTP hook 仍会运行；若出现卡住，记录现象，不在本次修改全局配置。
- **竖屏方向**：`setRotation(0)` 与 `(2)` 差 180°，烧录后按实际观感切换。

## 验收标准

1. 上述 7 项上机场景全部通过。
2. 离线测试与冒烟测试全过，固件编译零警告且不超 Flash/RAM。
3. `CLAUDE.md`、`README.md`、设计文档的七态与竖屏描述同步更新。
