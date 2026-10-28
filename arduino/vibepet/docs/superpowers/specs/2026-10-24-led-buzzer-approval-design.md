# LED 指示灯与审批提示音改造设计

日期：2026-10-24
状态：已与用户确认；已实现

> **实现后记（2026-10-27 核对）**：LED 语义在「失联 / 待审批」之外还包含
> **交互提醒**（`interactive` 态点亮）；蜂鸣器除审批三声短鸣外，交互提醒响两声、
> 批准两声、拒绝一声长鸣。真机已实测失联/恢复 LED、Bash 批准/拒绝与
> `PermissionRequest` 批准；三声短鸣的真机听感建议再确认一次。

## 背景

当前固件的 D5 指示灯语义是「链路正常时亮、失联时灭」；审批到达时蜂鸣器只响**一声
120 ms 长鸣**（`dispatchLine()` 里已有的 `beepStart(1, 120, 0)`），按键后另有提示音
（批准 2 声 35 ms、拒绝 1 声 120 ms）。这两个提示在「需要人介入」时反而不明显：
审批卡出现时屏幕之外没有醒目提示；失联时指示灯又是灭的，与「设备正常但没任务」无法区分。

## 需求

1. **LED（D5）**：改为「失联时亮、有待审批时亮」，其余时间熄灭。
2. **蜂鸣器（D4，无源）**：收到 `approval_request` 时响三声短鸣，提醒用户有审批。
3. **上机联调**：在真实 Claude Code 中测试多种审批类型的显示与按键
   （Bash 走 `PreToolUse`、文件写入走 `PermissionRequest`），覆盖批准、拒绝、
   长中文摘要折行与失联恢复。
4. 联调完成后，项目级 9 个 hook **保持启用**。

## 设计

### LED 语义

| 情形 | LED |
|---|---|
| 链路正常且无待审批 | 灭 |
| 有待审批（`ridBuf` 非空） | 亮 |
| 心跳超时、屏幕进入 LOST | 亮 |

实现位置：`checkWatchdog()` 末尾，把原来的
`digitalWrite(PIN_LED_STATUS, isLost ? LOW : HIGH);` 改为

```cpp
bool approvalPending = (ridBuf[0] != '\0');
digitalWrite(PIN_LED_STATUS, (isLost || approvalPending) ? HIGH : LOW);
```

进入失联时固件已经清空 `ridBuf`，恢复时审批卡也不会重放，因此两个条件互不干扰。
按键决策后 `ridBuf` 被清空，下一轮主循环 LED 自然熄灭。

### 审批提示音

在 `dispatchLine()` 的 `approval_request` 分支、字段全部解析完成之后，把现有的
`beepStart(1, 120, 0)` 替换为：

```cpp
beepStart(3, 35, 30);      // 审批到达：三声短鸣，约 0.5 秒（替换原一声 120ms）
setState(ST_NEEDS_YOU);    // 最后才重绘屏幕（铁律）
return;
```

- 复用现有的非阻塞节奏机（`tone()` 启动 + `beepService()` 每轮循环推进），
  **不引入 `delay()`**，不新增全局变量。
- 与现有按键音共存：审批到达「滴滴滴」；批准「滴滴」；拒绝「长滴」。
- 若连续收到审批请求，节奏机重新起算，从头响三声。

### 改动范围

- `firmware/VibePet_UNO/VibePet_UNO.ino`
  - 第 84 行 `PIN_LED_STATUS` 的注释
  - `checkWatchdog()` 末尾的 LED 写入
  - `dispatchLine()` 的 `approval_request` 分支
- 协议、电脑端（daemon / hook_client）、字库均无改动。

## 测试与验收

### 离线回归（不需要硬件）

- `g++` 离线协议测试：55 例必须全过（本次不碰解析代码，作为回归）。
- 固件编译零警告，Flash/RAM 预算不超。
- Python 四套冒烟测试作为电脑端回归。

### 上机联调（COM9）

顺序：停 daemon → 编译烧录 → 启动 daemon → 启用项目级 hook。

| # | 场景 | 预期 |
|---|---|---|
| 1 | Bash 审批，按**批准** | `APPROVE?` 卡片 + LED 亮 + 三声提示音；按 D2 → 命令放行、屏幕「已批准」、LED 灭 |
| 2 | Bash 审批，按**拒绝** | 按 D3 → 报 `Denied by VibePet`、命令未执行、屏幕「已拒绝」 |
| 3 | 文件写入（项目外）→ `PermissionRequest` | 设备显示审批卡；按批准 → 写入被放行 |
| 4 | 长中文摘要 | 摘要折行显示，不越界；未收录字显示空心框 |
| 5 | 审批等待期间收到状态更新 | 审批卡不被顶掉（回归用例 14 的实测版） |
| 6 | 停 daemon → 等 5 秒 | 屏幕 `LOST`，LED 亮；恢复 daemon 后回到正常画面、LED 灭 |

### 已知风险 / 不在本次范围

- Clawd on Desk 的 `PermissionRequest` HTTP hook 与 VibePet 并行等待的相互影响仍未验证
  （`.claude/README.md` 已记录）；文件写入测试时若出现卡住，需要单独排查。
- 交互类工具（`AskUserQuestion` / `ExitPlanMode`）不在本次测试范围。
- 失联测试后 daemon 需要重新启动；此时 hook 已启用，启动命令本身会被审批拦截，
  需先临时停用 hook 或由用户手动启动。

## 验收标准

1. 上述 6 个上机场景行为符合预期。
2. `g++` 离线测试 55 例全过，固件编译零警告。
3. 测试结束后项目级 9 个 hook 处于启用状态，daemon 可正常运行。
