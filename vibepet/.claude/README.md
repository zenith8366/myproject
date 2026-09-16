# VibePet 的 Claude Code Hook 配置

本目录的 `settings.json` 把 VibePet 挂在 Claude Code 的 6 个事件上，**目前全部处于注释状态，不会生效**。

## 为什么用 `# ` 而不是 JSON 注释

`settings.json` 是**严格 JSON**：`//` 和 `/* */` 都不被接受。而且后果比"忽略这一行"严重——
整个文件会被判定无效并**丢弃**，文件里的 hooks、permissions、statusLine 会**一起**失效。
官方文档明确写着不接受注释（"because Claude Code doesn't accept comments in a settings file"）。

所以这里走了另一条路：hook 的 `command` 值是交给 shell 执行的，而 shell 认 `#`。
配置本身是合法 JSON，命令却是被注释掉的：

```json
"command": "# python \"E:/myproject/vibepet/pc/hook_client.py\""
```

实测：bash 与 PowerShell 下执行该命令都是**退出码 0、stdout 零字节**，完全等同于这个 hook 不存在，
不会污染 Hook 通道，也不会产生报错噪音。

## 启用方式

把 `settings.json` 里 6 处 `"# python ..."` 的 `# `（井号 + 一个空格）删掉，变成：

```json
"command": "python \"E:/myproject/vibepet/pc/hook_client.py\""
```

保存即生效。在 Claude Code 里输入 `/hooks` 可以查看当前实际生效的 hook 列表。

## ⚠️ 启用前必须做到两件事

### 1. 先启动 daemon

```bash
python pc/bridge_daemon.py
```

VibePet 的降级策略是**拿不准就拒绝**。daemon 不在时，`PreToolUse` 会拒绝所有匹配的 Bash 调用——
这是安全设计而不是 bug，但会让你以为 Claude Code 坏了。

### 2. 注意与 Clawd on Desk 的并存

用户级配置 `~/.claude/settings.json` 里已经挂了一套 Clawd on Desk 的 hook，它同样占用
`PreToolUse`、`PostToolUse`、`Stop`、`SessionStart/End`、`UserPromptSubmit` 等事件。
Claude Code 会执行所有匹配的 hook，所以启用后两者**并存**：

- **状态上报类**（PostToolUse 等）互不干扰，两个设备都会跟着更新，无需处理；
- **`PreToolUse` 需要实测确认**：Clawd on Desk 的 timeout 只有 5 秒，而 VibePet 的审批最长 120 秒。
  两者都注册在同一事件上，最终行为取决于 Claude Code 对多个 hook 决策的合并策略。

## 各事件对应的行为

| 事件 | 行为 |
|---|---|
| `PreToolUse`（matcher: `Bash`） | 阻塞等待物理按钮，向 stdout 输出 allow / deny |
| `PostToolUse` | 上报 `working`；工具失败时报 `error` |
| `UserPromptSubmit` | 上报 `working` |
| `Stop` | 上报 `done` |
| `SessionStart` / `SessionEnd` | 上报 `idle` |

六种状态里的 `heartbeat_lost` 不在这里配——由设备端看门狗自行判断（5 秒收不到心跳即显示 `LOST`）。

## 禁用

把 `# ` 加回去，或直接删除本目录下的 `settings.json`。

## 这个文件为什么放在这里

`settings.json` 的**项目级**配置会与用户级**合并**（不是替换），所以 VibePet 的 hook 只在你
工作于 `E:\myproject\vibepet` 时才会被加载，切到其他项目不受影响。
