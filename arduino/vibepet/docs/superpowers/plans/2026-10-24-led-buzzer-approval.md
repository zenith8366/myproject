# LED 指示灯与审批提示音改造 Implementation Plan

> **执行后记（2026-10-27）**：本计划已执行。LED 现在在「失联 / 待审批 / 交互提醒」时点亮，
> 审批到达三声短鸣；交互提醒、批准、拒绝各有独立提示音。真机已实测失联/恢复 LED、
> Bash 批准/拒绝与 `PermissionRequest` 批准。计划里的 RAM 1286 等旧数字只代表执行当时。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 D5 指示灯在「失联」与「有待审批」时点亮，把审批到达提示音改为三声短鸣，并完成 COM9 真机多类型审批联调。

**Architecture:** 只改 `firmware/VibePet_UNO/VibePet_UNO.ino` 的两处运行时逻辑，复用现有非阻塞蜂鸣器节奏机；协议与电脑端零改动。

**Tech Stack:** Arduino UNO R3 / arduino-cli 1.5.1 / AVR core 1.8.8 / Python 3.14 + pyserial 3.5

## Global Constraints

- 设备端任何一处连续阻塞 ≤ 3 ms；禁用 `delay()`、`String`、动态内存。
- 固件编译必须带 `-DSERIAL_RX_BUFFER_SIZE=256` 与 `--build-path`，否则 `-D` 会静默失效。
- 烧录前、打开串口前必须停掉 daemon。
- hook 只改项目级 `.claude/settings.json`，**不得改动用户级全局配置**。
- 收尾时清点并删除过程产生的临时文件。
- 用户可见行为变化要同步 `README.md`。

## File Structure

- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino` —— 第 84 行引脚注释、第 879–881 行审批分支、第 1028–1029 行 LED 逻辑
- Modify: `README.md` —— 用户可见的提示行为描述
- Modify: `CLAUDE.md` —— 项目状态记录
- Create: `docs/superpowers/specs/2026-10-24-led-buzzer-approval-design.md`（已完成）
- Create: `docs/superpowers/plans/2026-10-24-led-buzzer-approval.md`（本文件）

---

## Task 1: 固件提示逻辑与离线回归

**Files:**
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino:84`
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino:879-881`
- Modify: `firmware/VibePet_UNO/VibePet_UNO.ino:1028-1029`
- Modify: `README.md:20`、`README.md:74`
- Modify: `CLAUDE.md` 的「当前状态」段

**Interfaces:**
- Consumes: 现有 `beepStart(uint8_t times, uint16_t on_ms, uint16_t off_ms)`、`ridBuf`、`isLost`
- Produces: 无新增接口；行为变化仅体现在 LED 与提示音

- [ ] **Step 1: 修改 LED 引脚注释**

```cpp
#define PIN_LED_STATUS  5   // 指示灯：失联或有待审批时点亮，其余时间熄灭
```

- [ ] **Step 2: 把审批到达提示音改为三声短鸣**

把 `dispatchLine()` 里的：

```cpp
    setState(ST_NEEDS_YOU);
    beepStart(1, 120, 0);   // 提示音：响一声，告诉用户「该你了」
    return;
```

改为：

```cpp
    beepStart(3, 35, 30);   // 审批到达：三声短鸣，告诉用户「该你了」
    setState(ST_NEEDS_YOU); // 最后才重绘屏幕（铁律：重绘会 pumpSerial）
    return;
```

- [ ] **Step 3: 修改 LED 判定**

把 `checkWatchdog()` 末尾的：

```cpp
  // 指示灯：链路正常才亮
  digitalWrite(PIN_LED_STATUS, isLost ? LOW : HIGH);
```

改为：

```cpp
  // 指示灯：失联（需要检查链路）或有审批等你按键时点亮，其余时间熄灭。
  // 进入失联时 ridBuf 已被清空，所以两个条件互不干扰。
  bool approvalPending = (ridBuf[0] != '\0');
  digitalWrite(PIN_LED_STATUS, (isLost || approvalPending) ? HIGH : LOW);
```

- [ ] **Step 4: 编译固件**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build firmware/VibePet_UNO
```

Expected: `Sketch uses ... bytes` / `Global variables use ... bytes`，零警告；
RAM 应仍为约 1286 字节（未新增全局变量），Flash 在 29784 字节上下小幅变化。

- [ ] **Step 5: 跑离线协议测试**

```bash
g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o /tmp/proto_test \
  && /tmp/proto_test
```

Expected: 55 例全部通过（输出以测试程序实际汇总行为准，不得有 failed）。

- [ ] **Step 6: 跑 Python 冒烟测试**

```bash
python pc/_smoke_test.py
python pc/_smoke_test_daemon.py
python pc/_smoke_test_state.py
python pc/_smoke_test_serial.py
```

Expected: 10 / 14 / 14 / 9 例全部通过。

- [ ] **Step 7: 更新 README 的用户可见描述**

第 20 行：

```markdown
在一块 1.77 英寸屏幕上；当 AI 需要批准敏感操作时，设备会响一声、弹出审批卡，你按一下设备
```

改为：

```markdown
在一块 1.77 英寸屏幕上；当 AI 需要批准敏感操作时，设备会响三声短鸣、弹出审批卡，
指示灯同时点亮，你按一下设备
```

第 74 行：

```markdown
| 指示灯 | 3 mm LED + 220 Ω 电阻 | 链路状态 |
```

改为：

```markdown
| 指示灯 | 3 mm LED + 220 Ω 电阻 | 失联 / 待审批时点亮 |
```

- [ ] **Step 8: 更新 CLAUDE.md 状态记录**

在「当前状态」列表末尾追加一条 2026-10-24 记录，写明：
LED 改为失联/待审批点亮；审批到达由一声 120 ms 改为三声 35 ms 短鸣；已上机联调
（或记录实际结果）。

- [ ] **Step 9: 提交（等待用户确认后执行）**

```bash
git add firmware/VibePet_UNO/VibePet_UNO.ino README.md CLAUDE.md \
  docs/superpowers/specs/2026-10-24-led-buzzer-approval-design.md \
  docs/superpowers/plans/2026-10-24-led-buzzer-approval.md
git commit -m "feat: 指示灯改为失联/待审批点亮，审批提示音改为三声短鸣"
```

---

## Task 2: COM9 真机联调

**Files:**
- Modify: `.claude/settings.json`（把 9 处 `"command": "# python ..."` 改为启用）
- Create: `C:\Users\lyh35\AppData\Local\Temp\vibepet_perm_test.txt`（PermissionRequest 测试文件）
- Modify: `CLAUDE.md`（记录联调结果）

**Interfaces:**
- Consumes: Task 1 烧录后的固件；`pc/bridge_daemon.py`；`pc/hook_client.py`
- Produces: 真机验收结果与启用的 hook 配置

> ⚠️ 顺序不能乱：编译、烧录、启动 daemon、失联测试都必须在**启用 hook 之前**完成。
> 一旦 hook 启用，所有 Bash 调用都会阻塞等待设备按钮。

- [ ] **Step 1: 确认端口与进程**

```bash
"/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe" board list
```

Expected: `COM9` 出现在列表中；任务管理器中无 `python pc/bridge_daemon.py` 进程。

- [ ] **Step 2: 编译并烧录 COM9**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build \
  --upload -p COM9 firmware/VibePet_UNO
```

Expected: 上传成功，无 `cannot open port`。

- [ ] **Step 3: 后台启动 daemon**

```bash
python pc/bridge_daemon.py
```

用后台方式运行（`run_in_background`），记录任务输出文件路径。

- [ ] **Step 4: 确认设备在线**

阅读 daemon 后台输出；按键测试前屏幕应显示 `IDLE`，daemon 日志出现端口打开与 `hello` 相关记录。

- [ ] **Step 5: 失联测试（hook 尚未启用）**

用 TaskStop 停止 daemon 后台任务，然后**等待至少 5 秒**观察设备：

Expected: 屏幕变为橙色 `LOST`，D5 指示灯**点亮**。

- [ ] **Step 6: 恢复测试**

重新后台启动 daemon，等待约 2 秒：

Expected: 屏幕恢复为 `IDLE`（失联前若为审批卡则退回 `IDLE`），D5 **熄灭**。

- [ ] **Step 7: 启用项目级 9 个 hook**

把 `.claude/settings.json` 中 9 处：

```json
"command": "# python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""
```

全部改为：

```json
"command": "python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""
```

可用一次 `Edit` 的 `replace_all` 完成。**不要写 JSON 注释**。

- [ ] **Step 8: Bash 批准路径（PreToolUse）**

调用 Bash 工具执行：

```bash
echo vibe-approve-test
```

Expected: 设备发出三声短鸣、显示 `APPROVE?` 与命令摘要、D5 点亮；在设备上按 **D2 批准**；
命令执行并输出 `vibe-approve-test`；屏幕显示「已批准」→ `WORKING`；LED 熄灭。

- [ ] **Step 9: Bash 拒绝路径（PreToolUse）**

再次调用 Bash 执行一条无害命令：

```bash
echo vibe-deny-test
```

Expected: 设备弹审批卡；按 **D3 拒绝**；Claude Code 报
`PreToolUse:Bash hook error: Denied by VibePet`，命令**未执行**；屏幕显示「已拒绝」。

- [ ] **Step 10: 文件写入路径（PermissionRequest）**

用 Write 工具创建项目外临时文件：

```
C:\Users\lyh35\AppData\Local\Temp\vibepet_perm_test.txt
```

内容：`vibepet permission request test`

Expected: 设备弹审批卡（`PermissionRequest` 路径）；按 **D2 批准**后文件创建成功。
若按键后 Claude Code 仍报拒绝或卡住，记录现象并检查 Clawd 并行 hook 的行为。

- [ ] **Step 11: 长中文摘要折行**

调用 Bash 工具执行：

```bash
echo "这是一条用于测试屏幕中文折行显示的长命令摘要请确认每一行都没有越过屏幕边界"
```

Expected: 审批卡摘要折行显示、不越界；未收录汉字显示空心方框（不显示乱码）。
按 **D3 拒绝**，命令被拦下。

- [ ] **Step 12: 审批卡不被状态顶掉（观察项）**

Bash 审批等待期间，daemon 仍可能收到状态上报。Expected: 审批卡保持显示直到按键或超时。
该行为已有离线回归（`pc/_smoke_test_daemon.py` 用例 14），上机以观察为准。

- [ ] **Step 13: 清理临时文件**

用 Bash 删除 `C:\Users\lyh35\AppData\Local\Temp\vibepet_perm_test.txt`
（会再触发一次审批，按批准即可），并确认仓库内无未跟踪的临时文件：

```bash
rm -f "/c/Users/lyh35/AppData/Local/Temp/vibepet_perm_test.txt"
git status --short
```

- [ ] **Step 14: 记录结果并提交（等待用户确认）**

把实测结果写入 `CLAUDE.md`（成功的场景、失败或待确认的场景），然后：

```bash
git add CLAUDE.md .claude/settings.json
git commit -m "chore: 启用项目级 VibePet hooks 并记录真机联调结果"
```

---

## Self-Review

- **Spec coverage:** LED 语义 → Task 1 Step 1/3；提示音 → Task 1 Step 2；README/CLAUDE 文档 → Task 1 Step 7/8；六项上机场景 → Task 2 Step 5–12；hook 保持启用 → Task 2 Step 7。
- **Placeholder scan:** 无 TBD/TODO；所有代码改动均给出完整替换内容。
- **Type consistency:** 仅复用既有 `beepStart` / `ridBuf` / `isLost`，无新签名。
