# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库现状

- `pc/hook_client.py` —— Hook 客户端（短生命周期），仅用标准库。两条行为完全不同的路径：`PreToolUse` 审批（阻塞、向 stdout 输出决策）与其余事件的状态上报（非阻塞、stdout 零输出）
- `pc/bridge_daemon.py` —— 桥接守护进程（常驻）。真实模式用 `bleak`（已装 3.0.2），`--no-ble` 模式不需要它
- `pc/_smoke_test.py`（7 例）、`pc/_smoke_test_daemon.py`（12 例）、`pc/_smoke_test_state.py`（14 例）—— 冒烟测试，**均不依赖硬件或蓝牙**
- **三个核心文件的注释是刻意写给新手看的**（`pc/hook_client.py`、`pc/bridge_daemon.py`、`firmware/VibePet/VibePet.ino`）：每个函数前有一段大白话说明「这段在干嘛、为什么需要它」，容易卡住新手的行再逐行拆解。注释密度高于一般工程代码，这是本项目目标读者（没写过嵌入式 / 异步程序的人）决定的 —— **不要以「注释太多/太啰嗦」为由删减**
- `README.md` —— **面向使用者**的文档（安装 / 日常使用 / 排障），受众与 CLAUDE.md 不同。改了用户可见的行为（命令行参数、状态含义、安装步骤）要同步更新它
- `VibePet —— AI 编程助手物理状态显示与审批终端（无线 BLE 版）v2.0.md`（位于仓库根目录，文件名含中文、空格与全角破折号，引用时务必加引号）

这份文档是**唯一权威规格**，涵盖需求、硬件选型与接线、通信协议、软件模块划分、示例代码、开发计划、测试方案与风险分析。开始任何实现前先读它。本文件只提炼跨章节阅读才能得出的约定，不替代文档；两者冲突时以设计文档为准。

- `firmware/VibePet/VibePet.ino` —— ESP32-C3 固件（881 行，其中约三分之一是面向新手的注释）。**已通过编译验证**（`esp32:esp32` 3.3.11，零警告）
- `firmware/TFT_eSPI_User_Setup.h` —— TFT_eSPI 配置模板。**它不是编译单元**，是给库的 `User_Setup.h` 覆盖用的
- `.claude/settings.json` —— Claude Code Hook 配置，挂在 6 个事件上，但**当前 6 处 command 全部以 `#` 注释着，不会生效**。启用方式与冲突提醒见 `.claude/README.md`

**固件尚未在真实硬件上验证**（没有设备）。已确认的只有：能编译、Flash 占用 27%（Huge APP 分区，含约 200 KB 中文点阵字库）、协议字面量与电脑端逐一对齐、用到的 TFT_eSPI / U8g2_for_TFT_eSPI API 签名正确。**未确认**：TFT 初始化序列是否匹配你的模块（`ST7735_INITB` 可能需要换）、接线与引脚、真实 BLE 行为、屏幕布局与中文渲染的实际观感（wqy12 行高约 15px，摘要行数与坐标按此估算，上机后可能需微调）。

**设计文档 5.5 节的示例代码是示意片段，不是可用实现，不要照抄**：daemon 部分缺断线重连、并发保护（F9 单槽会被第二个请求覆盖）与审批后状态复位（屏幕会永远停在 `APPROVE?`）；hook_client 部分缺 socket 超时（会让 Claude Code 永久卡死）、半关闭（与 daemon 的 `reader.read()` 互等死锁）和 UTF-8 处理。

## 常用命令

```bash
# 冒烟测试（都不需要硬件或蓝牙）
python pc/_smoke_test.py           # hook_client 审批路径：7 例
python pc/_smoke_test_daemon.py    # bridge_daemon：12 例，含端到端联调
python pc/_smoke_test_state.py     # 事件 → 状态映射：14 例

# 语法检查
python -m py_compile pc/hook_client.py pc/bridge_daemon.py

# 编译固件（用 D 盘 Arduino IDE 自带的 cli；路径含空格必须加引号）
# FQBN 必须带 PartitionScheme=huge_app：中文点阵字库约 200 KB，默认分区放不下
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" \
  --build-property 'compiler.cpp.extra_flags=-DUSER_SETUP_LOADED=1 -DST7735_DRIVER -DTFT_WIDTH=128 -DTFT_HEIGHT=160 -DTFT_CS=7 -DTFT_DC=8 -DTFT_RST=5 -DTFT_MOSI=6 -DTFT_SCLK=4 -DLOAD_GLCD -DSPI_FREQUENCY=27000000 -DST7735_INITB' \
  firmware/VibePet

# 烧录（COM 口号换成你实际的）
"$CLI" upload --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" -p COM3 firmware/VibePet

# 无硬件手动联调：--no-ble 模式下按钮事件改由本地 Socket 注入
python pc/bridge_daemon.py --no-ble        # 终端 A
echo '{"tool_name":"Bash","tool_input":{"command":"ls"}}' | python pc/hook_client.py   # 终端 B，会阻塞
# 终端 C：先发 {"type":"status"} 取回 pending_request_id，再发
#         {"type":"button","request_id":"<该值>","action":"approve"}
```

电脑端要求 Python ≥ 3.9（开发机实测 3.14.7）。`hook_client.py` 仅用标准库；`bridge_daemon.py` 只在真实 BLE 模式下需要 `pip install bleak`（缺失时启动即报错退出，不会静默空转）。

Hook 配置**已经写好并放在 `.claude/settings.json`**（项目级，只在本项目生效，与用户级配置合并），
但 6 个事件的 command 全部以 `#` 开头注释着，**当前不生效**。启用方式、与 Clawd on Desk 的冲突提醒见 `.claude/README.md`。

**启用 Hook 前必须先启动 daemon** —— 否则 daemon 缺席，所有匹配的 Bash 调用都会被拒绝。

**绝对不要往 `settings.json` 里写 JSON 注释**：Claude Code 用严格 JSON 解析，一个 `//` 会让**整个文件被丢弃**，
连带停用文件里的所有 hooks / permissions / statusLine（不是只忽略那一行）。要"注释掉"一段 hook，
正确做法是注释 `command` 的**值**——也就是加 shell 注释符 `#`，因为该值最终是交给 shell 执行的。
实测 bash 与 PowerShell 下 `# ...` 都是退出码 0、stdout 零字节的安全 no-op。

## 项目是什么

一个桌面实体设备：通过 BLE 连接电脑，实时显示 Claude Code 等 AI 编程助手的工作状态，并提供两个物理按钮完成 approve / deny 审批，让开发者不必盯着终端。

## 架构：三层 + 一条 BLE 链路

```
Claude Code (PreToolUse Hook)
   │ stdin: Hook JSON
   ▼
hook_client.py          短生命周期 —— 每次 Hook 触发启动一次进程
   │ 本地 TCP socket 127.0.0.1:8765，阻塞等待决策
   ▼
bridge_daemon.py        常驻进程 —— BLE 连接管理 / 心跳 / request_id 校验 / 审批等待
   │ BLE NUS，JSON Lines
   ▼
ESP32-C3 固件 main_ble.ino   NimBLE + TFT_eSPI + ArduinoJson
```

最核心的设计决策是**短生命周期的 hook_client 与常驻 daemon 分离**：BLE 连接、扫描、重连、心跳只能由 daemon 独占管理（BLE 连接建立耗时以秒计，无法承受每次 Hook 调用重建）。hook_client 每次只做一次 socket 往返，且必须在 daemon 缺席时安全降级而非崩溃。

设备端是**主循环轮询 + BLE 回调**：JSON 解析在 NimBLE 的 `onWrite` 回调里完成，主循环（约 10 ms 一轮）负责看门狗、按钮扫描与动画刷新。

## 通信协议（硬契约，任何改动必须两端同步）

BLE Nordic UART Service 作为透明串口，承载 JSON Lines —— 每条消息以 `\n` 结尾。

| 角色 | UUID |
|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX（电脑 → 设备，Write/WriteNR） | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX（设备 → 电脑，Notify） | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

设备广播名 `VibePet`，电脑端扫描时按名字子串匹配。MTU 协商到 185，但**接收端（含设备端）仍必须按 `\n` 重组缓冲区**——分片是常态而非异常。

**行长是硬约束**：整行 UTF-8 ≤ **512 字节**（固件 `LINE_MAX`，超长整条丢弃、不会把半截行当消息处理）；电脑端单字段（`summary`/`msg`）≤ **240 字节**，截断必须走 `clamp_bytes()`（按 UTF-8 边界，不切碎汉字）——60 个汉字 = 265 字节，整条 `approval_request` 会被设备丢弃。

电脑 → 设备：

```json
{"type":"state","status":"working","msg":"正在分析代码..."}
{"type":"approval_request","request_id":"abc123","tool":"Bash","summary":"rm -rf /tmp/build"}
{"type":"heartbeat","seq":12345}
```

设备 → 电脑：

```json
{"type":"button","request_id":"abc123","action":"approve"}
```

六种状态值（`status` 字段的取值域）：

| 状态值 | 颜色 | 屏幕效果 |
|---|---|---|
| `idle` | 白 | 呼吸圆点 + "IDLE" |
| `working` | 绿 | 旋转方块 + "WORKING" |
| `needs_you` | 黄 | 闪烁边框 + "APPROVE?" + 命令摘要 |
| `done` | 青 | 对勾 + "DONE" |
| `error` | 红 | 叉号 + "ERROR" + 错误信息 |
| `heartbeat_lost` | 橙 | "LOST" 大字 |

## Hook 事件 → 状态映射

`hook_client.py` 会被挂在多个 Hook 上，按 `hook_event_name` 分派。六种状态中
`heartbeat_lost` 由设备端看门狗独立判断，其余五种在这里闭环：

| Hook 事件 | 状态 | 屏幕文字 |
|---|---|---|
| `PreToolUse` | 不发 state | 改走 `approval_request`，阻塞等按钮 |
| `PostToolUse` 成功 | `working` | `<工具名> 完成` |
| `PostToolUse` 失败 | `error` | `<工具名> 出错` |
| `SessionStart` / `SessionEnd` | `idle` | 会话开始 / 会话结束 |
| `UserPromptSubmit` | `working` | 思考中 |
| `SubagentStop` | `working` | 子任务完成 |
| `PreCompact` | `working` | 压缩上下文 |
| `Stop` | `done` | 任务完成 |
| 其他事件 | 不上报 | —— |

**两条路径的行为差异是刻意的，改动时不要混为一谈**：

- `PreToolUse` 阻塞等按钮（最长 120 s），**必须**向 stdout 输出 allow / deny。
- 状态事件超时只有 1 s，失败静默放弃，**stdout 必须零字节** —— 它挂在每一次工具
  调用的路径上，多耗一秒就是实打实地拖慢 Claude Code；多余输出则会污染 Hook 通道。

`PostToolUse` 的成功/失败判定是**启发式的**：只看 `is_error` 与 `error` 两个字段。
Claude Code 各工具的 `tool_response` 结构并不统一，也没有稳定的通用错误字段，
判不出的按成功处理 —— 宁可不报错，也不要误报红色 ERROR。

## 时序与常量

| 常量 | 值 | 位置 |
|---|---|---|
| 心跳间隔 | 1.0 s | daemon 主动发送 |
| 看门狗超时 | 5000 ms | 设备端 `WATCHDOG_TIMEOUT` |
| 按钮去抖 | 200 ms | 设备端 `DEBOUNCE_MS` |
| 审批超时 | 120 s（可配置） | daemon，超时返回 `deny` |
| 本地 Socket | `127.0.0.1:8765` | `hook_client.py` ↔ daemon |
| 动画刷新 | ~100 ms | 设备端主循环 |
| BLE 行上限 | 512 字节 | 设备端 `LINE_MAX`，超长整条丢弃 |
| 单字段上限 | 240 字节 | daemon `MAX_FIELD_BYTES`（summary / msg） |

## 硬件接线（ESP32-C3 SuperMini）

显示屏为 **1.77" ST7735S（128×160）**，`tft.setRotation(1)` 的横屏画布是 **160×128**。设计文档 5.5 节的示例代码坐标按旧的 80×160 屏幕书写，不能直接照搬居中效果。

| 外设 | GPIO | 说明 |
|---|---|---|
| TFT CS | 7 | ST7735S 片选 |
| TFT SDA/MOSI | 6 | |
| TFT SCK | 4 | |
| TFT A0/DC | 8 | |
| TFT RES | 5 | |
| 批准按钮 | 1 | `INPUT_PULLUP`，按下为 LOW |
| 拒绝按钮 | 10 | `INPUT_PULLUP`，按下为 LOW |
| 蜂鸣器 | 3 | 无源，须 PWM 方波驱动（`tone()`），空闲保持 LOW |
| LED 指示灯 | 2 | BLE 连接状态 |

**引脚选择本身就带避坑意图，不要随意更换**：GPIO9 是 ESP32-C3 的启动引脚，必须避开；按钮接 `INPUT_PULLUP` 且选中上述引脚，是为了保证按住按钮上电时设备仍能正常启动、不误入下载模式（设计文档 4.3 的引脚确认表要求实测这一点，7.2 有对应用例）。

**完整接线**（每个元件的第二根线接哪、接线顺序、上电前检查清单）见**设计文档 4.7**；上面这张表只列 GPIO 分配，给人快速查阅用。4.7 是接线的唯一权威，改引脚时以它为准。

## 硬性约束

- **`hook_client.py` 的 stdout 必须只输出 Hook JSON**。所有调试信息一律写 stderr。stdout 一旦被日志污染，Hook 解析失败会直接破坏审批链路——这是设计文档 8.1 列为「高影响」的风险。
- **Windows 编码**：Python 文本流在中文 Windows 上默认跟随系统 locale（GBK），一旦写入非 GBK 字符就抛 `UnicodeEncodeError`，会让 Hook 进程整个崩掉。`hook_client.py` 因此显式把 stderr reconfigure 成 UTF-8，并用 `sys.stdout.buffer` 直接写 UTF-8 字节。**新增的电脑端脚本必须沿用这个模式**，不要用裸 `print()` 往 stdout/stderr 写非 ASCII 内容。
- **`request_id` 一一对应**。daemon 为每次审批生成唯一 id 并只接受当前等待中的那个，设备回传时必须原样带回。目的是让迟到按钮（上一次审批超时后才按下）不会误批准下一次请求。这个机制从第一版就要有，后期补会引发重构。
- **降级方向一律是拒绝**。审批超时 → `deny`；daemon 连接失败 / 超时 / 解析失败 → `deny`。任何异常都不得让 Hook 崩溃或放行。
- **按钮按下后设备端立即本地切画面**（批准 → `working`/“已批准”，拒绝 → `idle`/“已拒绝”）并作废 `request_id`，不依赖电脑端回话。daemon 的去重模型是「记账 + dirty」（`_last_state` / `_state_dirty`），重连后由 `resend_current()` 补发当前画面：审批进行中重发审批卡（**沿用原 request_id**），否则重发最后一条 state。设备端 `lastValidState/lastValidMsg` 只记非 LOST 状态。
- **LOST 恢复语义**：恢复到最后有效状态；若那是 `needs_you` 则恢复为 `idle`（审批请求可能已超时失效，不显示过期卡片）。进入 LOST 时清空 `request_id`，失联期间按键不回传。
- **中文显示**：正文（摘要 / msg / 副标题）用 U8g2_for_TFT_eSPI 的 `wqy12_t_gb2312` 字体（`FONT_BODY`，约 200 KB Flash，必须 Huge APP 分区）；标题类大字仍是 GLCD。改字号或换字体会同时影响 Flash 占用与分区要求。
- **同一时间只处理一个审批请求**（单槽 `current_request_id`，需求 F9）。多会话 FIFO 队列属于可扩展方向，不在第一版。
- **第一版仅 USB 5V 供电**，锂电池是可选加分项。设计文档 4.4 明确警告：电池必须接开发板 5V/VBUS 脚经板载 LDO，**不得直连 3.3V 引脚**。

## 工具链与烧录配置

| 项目 | 选型 |
|---|---|
| 嵌入式环境 | Arduino CLI 1.5.1（D 盘 Arduino IDE 自带）或 Arduino IDE |
| ESP32 核心 | **`esp32:esp32` 3.3.11**（实测版本；设计文档写的 ≥3.0 是底线） |
| BLE 库 | **NimBLE-Arduino 2.5.1**（而非 ESP32 原生 BLE 库，省 RAM 与 Flash）。2.x 的回调签名带 `NimBLEConnInfo&` 参数，写成 1.x 的 `onConnect(NimBLEServer*)` 会编译失败 |
| 中文字体 | **U8g2 2.36.19 + U8g2_for_TFT_eSPI 1.7.0** —— 正文用 `u8g2_font_wqy12_t_gb2312`（约 200 KB Flash）。U8g2_for_TFT_eSPI 不在库管理器索引里，需 `git clone https://github.com/Bodmer/U8g2_for_TFT_eSPI` 到 libraries 目录 |
| TFT 驱动 | TFT_eSPI 2.5.43（Bodmer 版），实际位于 `E:\Users\lyh35\Documents\Arduino\libraries` —— **库目录在 E 盘不是 C 盘**，找库时别找错 |
| JSON 库 | ArduinoJson **7.4.3**（设计文档写的是 v6，实测 v7 亦可）。注意 v7 里 `StaticJsonDocument<N>` 只是 `JsonDocument` 的兼容壳：**池是动态的**（堆分配、按需增长），`<N>` 既不限制也不预留内存 —— 与 v6 的「栈上定长池」语义不同，别指望它兜住内存上限；真正的上限由协议保证（整行 ≤ 512 字节） |
| 电脑端 | Python ≥ 3.9 + `bleak` 3.0.2 + `asyncio` |

Arduino IDE 关键配置（改动后需重新确认，并备份 TFT_eSPI 的 `User_Setup.h`）：开发板 `ESP32C3 Dev Module`、USB CDC On Boot `Enabled`、Partition Scheme `Huge APP (3MB No OTA/1MB SPIFFS)`、Flash Size `4MB`、Upload Speed `921600`。

TFT 引脚不走代码 `#define`，而是在 TFT_eSPI 的 `User_Setup.h` 中配置（选 `ST7735_DRIVER`、分辨率 128×160；初始化序列 `ST7735_INITB` / `ST7735_GREENTAB` 等因厂商而异，须按实际模块实测）——排障屏幕不亮时优先查这里。

## 调试顺序

设计文档 8.3 规定自底向上分步验证，**每步通过后再进入下一步**，不要在链路未验证时联调：

TFT 点亮 → BLE 广播（nRF Connect 扫描到 `VibePet`）→ BLE 收发（nRF Connect 手写 JSON，看屏幕切状态、按钮回传）→ daemon 独立测试 → hook_client 命令行测试 → 接入 Claude Code Hook 端到端。

`bleak` 平台注意：Windows 需 10 build 16299 以上；优先在 macOS/Linux 开发可减少兼容性问题。
