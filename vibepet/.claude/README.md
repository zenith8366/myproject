# VibePet 的 Claude Code Hook 配置

本目录的 `settings.json` 把 VibePet 挂在 Claude Code 的 8 个事件上，**当前处于启用状态**
（2026-09-25 启用并实测通过）。

## 各事件与超时

| 事件 | matcher | 行为 | timeout |
|---|---|---|---|
| `PreToolUse` | `Bash` | **阻塞**等待设备按钮，向 stdout 输出 allow / deny | **150 秒** |
| `PostToolUse` | —— | 上报 `working`；工具失败时报 `error` | 5 秒 |
| `UserPromptSubmit` | —— | 上报 `working`（思考中） | 5 秒 |
| `Stop` | —— | 上报 `done`（任务完成） | 5 秒 |
| `SessionStart` / `SessionEnd` | —— | 上报 `idle` | 5 秒 |
| `SubagentStop` | —— | 上报 `working`（子任务完成） | 5 秒 |
| `PreCompact` | —— | 上报 `working`（压缩上下文） | 5 秒 |

**关于 `timeout`**：这三个字不能省。不写的话走的是默认超时（比 VibePet 的 120 秒审批窗口
还短），等待会被提前掐死 —— 那时 `hook_client.py` 来不及输出决策，等于白白丢掉一次审批。
`PreToolUse` 给 **150 秒**是刻意留出的余量：客户端自己等 130 秒、daemon 等 120 秒，
150 > 130 > 120，保证「超时降级为拒绝」这出戏由 daemon 演完，而不是被外部杀掉。

状态类事件给 5 秒：它们挂在每一次工具调用的路径上，绝不能拖慢 Claude Code
（`hook_client.py` 内部实际只等 1 秒就放弃）。

## 启用与关闭

**关闭**（临时禁用某个或全部 hook）：把对应 `command` 的值前面加 `# `，例如

```json
"command": "# python \"E:/myproject/vibepet/pc/hook_client.py\""
```

为什么可以这么注释：hook 的 `command` 值是**交给 shell 执行的**，而 shell 认 `#`。
实测 bash 下 `# ...` 是退出码 0、stdout 零字节的安全 no-op。

> ⚠️ **绝对不要往 `settings.json` 里写 JSON 注释**（`//` 或 `/* */`）。Claude Code 用严格
> JSON 解析：一个 `//` 会让**整个文件被丢弃**，连带里面的 hooks / permissions / statusLine
> 全部失效 —— 不是只忽略那一行。要注释就注释 `command` 的值。

**查看当前生效的 hook**：在 Claude Code 里输入 `/hooks`。

## ⚠️ 启用前必须做到三件事

### 1. 先启动 daemon

```bash
python pc/bridge_daemon.py
```

VibePet 的降级策略是**拿不准就拒绝**。daemon 不在时，`PreToolUse` 会拒绝所有匹配的 Bash
调用 —— 这是安全设计而不是 bug，但会让你以为 Claude Code 坏了。

### 2. 烧录设备前先停掉 daemon

设备那根串口**同时是数据链路和烧录口**，同一时刻只能被一个程序拿着：

- 烧录前：在 daemon 窗口按 `Ctrl+C`，烧完再启动；
- 用 Arduino IDE 的**串口监视器**调试设备时同理 —— 开着它，daemon 就打不开串口；
- 不听劝的后果实测过：`avrdude: cannot open port COM9: 拒绝访问`，就是 daemon 占着。

顺带一提：**打开串口会让 UNO 复位一次**（USB 转串口芯片拉 DTR），约 2 秒后进入固件。
这不是故障，daemon 会等它启动完再把当前画面补发过去。

### 3. 与 Clawd on Desk 并存（已确认不冲突）

用户级配置 `~/.claude/settings.json` 里挂着 Clawd on Desk 的一整套 hook，事件几乎完全重叠。
**结论：两者可以并存，不会抢决策权**，原因有两条：

1. **Clawd 的 hook 全部带 `"async": true`** —— 后台执行、不阻塞、不产生决策，只是给自己的
   界面推状态。VibePet 的 `PreToolUse` 是唯一做决策的那个。
2. Clawd 另有一个 `PermissionRequest` 的 HTTP hook（`127.0.0.1:23333`）。它和 VibePet 用的是
   **两套不同机制**：VibePet 在 `PreToolUse` 阶段就给出 allow / deny，一旦 allow，
   Claude Code 直接放行、不会走到权限询问那一步（所以 Clawd 的权限弹窗也不会出现）。
   想要 Clawd 的弹窗，就得先把 VibePet 的 hook 关掉。

## 实测记录（2026-09-25）

| 验证项 | 结果 |
|---|---|
| 配置文件被加载 | ✅ 改完不重启也生效（Claude Code 监听 `.claude/`） |
| hook 里 `python` 可用 | ✅ 标记文件记下 `hook_client 退出码=0` |
| **allow 路径** | ✅ 设备按批准 → 命令放行执行 |
| **deny 路径** | ✅ 设备按拒绝 → 报 `PreToolUse:Bash hook error: Denied by VibePet`，命令未执行 |

**按钮位置提醒**：实测发现，接在 **D2** 上的按钮是「批准」（屏幕显示**已批准**），
接在 **D3** 上的是「拒绝」（屏幕显示**已拒绝**）。两个按钮哪个在左哪个在右取决于你怎么接的线 ——
拿不准就在一次审批进行中按一下，屏幕会立刻告诉你它是哪个。想让它们对调：把两根杜邦线
在 D2/D3 上换一下即可（固件不用动）。

## 这个文件为什么放在这里

`settings.json` 的**项目级**配置会与用户级**合并**（不是替换），所以 VibePet 的 hook 只在你
工作于 `E:\myproject\vibepet` 时才会被加载，切到其他项目不受影响。
