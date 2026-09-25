# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 当前状态：v3.0（UNO R3 + USB 串口）已实现，待上机联调

- **设计文档 v3.0**（`VibePet —— AI 编程助手物理状态显示与审批终端（USB 有线版）v3.0.md`，位于仓库根目录，文件名含中文、空格与全角破折号，引用时务必加引号）。它是**唯一权威规格**，开始任何实现前先读它。
- **已实现并有验证**（2026-09-25）：
  - `firmware/VibePet_UNO/`：`VibePet_UNO.ino`（857 行）+ `vibepet_proto.h`（协议内核）+ `cn_font.h`（生成物）。**编译零警告**，Flash 27636/32256（85%）、RAM 1282/2048（62%）
  - `tools/gen_cn_font.py`：字库裁剪工具，`--verify` 黄金字形自检通过（`中` / `A` 逐位一致）
  - `tools/test_proto.cpp`：协议内核的**离线测试（52 例）**，用 g++ 在电脑上跑，不需要硬件
  - `pc/bridge_daemon.py` 的 `SerialTransport` + `pc/_smoke_test_serial.py`（9 例）
  - Python 侧四套冒烟测试共 **43 例全通过**
- **屏幕已验证点亮**：`test-firmware/tfttest_uno` 在这块 UNO + 1.77" 屏上显示正常（用户实测）。
- **尚未做**：固件在真机上的端到端联调（烧录、串口收发、按钮、看门狗、中文渲染的实机观感）；设计文档里标「待实测」的性能数字。
- **v2.0 遗留已清理**（2026-09-25）：删掉了 `firmware/VibePet/`（901 行 ESP32 固件）、`vendor/TFT_eSPI/`（274 个文件，占仓库跟踪文件数的 94%）、`firmware/TFT_eSPI_User_Setup.h`、`test-firmware/tft_probe{,2}/`。仓库跟踪文件从 295 个降到 17 个，只剩有线版一条路线；要查旧实现请翻 git 历史。

## 仓库现状

- `pc/hook_client.py` —— Hook 客户端（短生命周期），仅用标准库。两条行为完全不同的路径：`PreToolUse` 审批（阻塞、向 stdout 输出决策）与其余事件的状态上报（非阻塞、stdout 零输出）。**本文件与传输方式无关，从 v2.0 到 v3.0 一行都不用改**
- `pc/bridge_daemon.py` —— 桥接守护进程（常驻）。传输层是**鸭子类型的 4 方法契约**（`set_line_handler` / `connected` 属性 / `async ensure_connected` / `async send_line`），`Bridge` 与全部审批逻辑对此无感；`SerialTransport` 是串口实现（后台线程读 + `call_soon_threadsafe` 送回事件循环）
- `firmware/VibePet_UNO/` —— UNO 固件与协议内核（**注释是写给新手看的，见下方说明**）
- `pc/_smoke_test.py`（7 例）、`pc/_smoke_test_daemon.py`（13 例）、`pc/_smoke_test_state.py`（14 例）、`pc/_smoke_test_serial.py`（9 例）—— 冒烟测试，**均不依赖硬件**
- `tools/test_proto.cpp` —— 协议内核的离线测试（52 例），g++ 编译即跑，**也不需要硬件**
- `test-firmware/tfttest_uno/tfttest_uno.ino` —— **UNO + ST7735 的接线与库用法权威参考**（Adafruit_GFX + Adafruit_ST7735，引脚 CS=D10 / DC=D9 / RES=D8 / SCK=D13 / MOSI=D11 / 背光→3.3V）。主固件的显示部分照它写，且已实测点亮
- `tools/cn_charset.txt` —— 字库字集清单；`tools/gen_cn_font.py` —— 字库裁剪工具
- `README.md` —— **面向使用者**的文档（安装 / 日常使用 / 排障），受众与 CLAUDE.md 不同。改了用户可见的行为（命令行参数、状态含义、接线、安装步骤）要同步更新它
- `.claude/settings.json` —— Claude Code Hook 配置，挂在 6 个事件上，但**当前 6 处 command 全部以 `#` 注释着，不会生效**。启用方式与冲突提醒见 `.claude/README.md`

**注释密度是刻意的，不要删**：`pc/hook_client.py`、`pc/bridge_daemon.py`、`firmware/VibePet_UNO/VibePet_UNO.ino` 三个核心文件的注释是写给新手看的（每个函数前有一段大白话说明「这段在干嘛、为什么需要它」），密度高于一般工程代码。这是本项目目标读者（没写过嵌入式 / 异步程序的人）决定的 —— **不要以「注释太多/太啰嗦」为由删减**。

## 常用命令

```bash
# 冒烟测试（都不需要硬件）
python pc/_smoke_test.py           # hook_client 审批路径：7 例
python pc/_smoke_test_daemon.py    # bridge_daemon：13 例，含端到端联调
python pc/_smoke_test_state.py     # 事件 → 状态映射：14 例
python pc/_smoke_test_serial.py    # 串口传输层：9 例，注入假串口，不需要真设备

# 上机调试：串口探针（手工给设备发 JSON，相当于 v2.0 时代的 nRF Connect）
# 用它之前先停掉 daemon；打开串口会让板子复位，等约 2 秒
python tools/serial_probe.py COM7
#   /state working 正在分析代码     /state needs_you 要删缓存了
#   /approve  /deny  /heartbeat  /raw {"type":"state"}
#   直接敲一行 JSON 回车也行；设备回传的每一行都会带时间戳打印出来

# 协议内核的离线测试（也不需要硬件！用电脑上的 g++ 编译固件里的那份解析代码）
# 可执行文件写到临时目录 —— 别落在仓库里，那会平白多个未跟踪文件
g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o /tmp/proto_test \
  && /tmp/proto_test               # 52 例：转义、\uXXXX、嵌套、UTF-8 边界、行重组

# 语法检查
python -m py_compile pc/hook_client.py pc/bridge_daemon.py

# ── 编译固件（arduino:avr 核心 1.8.8，开发板 Arduino Uno）──
# ★ -DSERIAL_RX_BUFFER_SIZE=256 不能省：AVR 默认串口接收缓冲只有 64 字节，
#   而一条审批请求最长 320 字节，配合「连续阻塞 ≤ 3ms」的纪律才够用
# ★ --build-path 也不能省：不带它时这条 -D 会**静默失效**（见下方说明）
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build firmware/VibePet_UNO

# ── 也可以用 Arduino IDE 的「上传」按钮（本项目没有 --libraries 之类的坑）──
#   IDE 读的是同一份 Arduino 核心，唯一差别是串口接收缓冲 64 而不是 256 字节：
#   按固件「连续阻塞 ≤ 3ms」的纪律，最多只会攒下约 29 字节，64 够用。
#   代价与收益：缓冲小的那份反而给栈多留了 192 字节。两条路都能用。
#
# 编译 + 烧录（COM 口号用 arduino-cli board list 查；烧录前必须先停掉 daemon）
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build \
  --upload -p COM7 firmware/VibePet_UNO

# ── 验证 -D 真的生效了：看 RAM 数字 ──
#   带 -D：Global variables ≈ 1282 字节（RX 256 + TX 64）
#   不带：Global variables ≈ 1090 字节（RX 64）  ← 数字不对说明标志没生效
#
# 为什么必须显式给 --build-path：arduino-cli 会缓存核心库（HardwareSerial 等）
# 的编译产物，而**这份缓存的键不包含 build.extra_flags**。用默认临时目录编译时
# 会命中一份没带标志的旧缓存，于是 SERIAL_RX_BUFFER_SIZE 悄悄退回 64 ——
# 编译不报错、行为变差，属于最难查的那类问题。指定自己的构建目录后缓存与标志
# 一一对应，实测连续三次编译结果稳定。

# 编译后想看 Flash/RAM 究竟被谁吃了（比总数字有用得多）
AVR="C:/Users/lyh35/AppData/Local/Arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin"
"$AVR/avr-nm.exe" --size-sort -S firmware/VibePet_UNO/build/VibePet_UNO.ino.elf | tail -25

# 重新生成中文字库（改了 tools/cn_charset.txt 或 hook_client 的设备可见文案之后）
python tools/gen_cn_font.py --verify      # 先自检：黄金字形逐位比对
python tools/gen_cn_font.py --out firmware/VibePet_UNO/cn_font.h

# 无硬件手动联调：--no-device 模式下按钮事件改由本地 Socket 注入
python pc/bridge_daemon.py --no-device     # 终端 A
echo '{"tool_name":"Bash","tool_input":{"command":"ls"}}' | python pc/hook_client.py   # 终端 B，会阻塞
# 终端 C：先发 {"type":"status"} 取回 pending_request_id，再发
#         {"type":"button","request_id":"<该值>","action":"approve"}
```

电脑端要求 Python ≥ 3.9（开发机实测 3.14.7；机器上只有一个解释器，`python` 即可，不存在多环境挑错的问题）。`hook_client.py` 仅用标准库；`bridge_daemon.py` 只在真实串口模式下需要 `pip install pyserial`（已装 3.5；缺失时启动即报错退出，不会静默空转）。

**离线测试需要一个本机 C++ 编译器**（`g++`，开发机实测 MinGW 16.2.0）。它只用于 `tools/test_proto.cpp`；编译固件用的是 arduino-cli 自带的 avr-gcc，两者互不影响。

## 项目是什么

一个桌面实体设备：**通过一根 USB 线连接电脑**，实时显示 Claude Code 等 AI 编程助手的工作状态，并提供两个物理按钮完成 approve / deny 审批，让开发者不必盯着终端。

设备由 USB 供电，**不支持电池独立工作**（数据链路就是那根线）。

## 架构：三层 + 一条 USB 串口链路

```
Claude Code (PreToolUse Hook)
   │ stdin: Hook JSON
   ▼
hook_client.py          短生命周期 —— 每次 Hook 触发启动一次进程
   │ 本地 TCP socket 127.0.0.1:8765，阻塞等待决策
   ▼
bridge_daemon.py        常驻进程 —— 串口管理 / 心跳 / request_id 校验 / 审批等待
   │ USB 串口 115200，JSON Lines（设备表现为 COM 口）
   ▼
Arduino UNO R3 固件 VibePet_UNO.ino   Adafruit_ST7735 + 自制 12px 中文子集字库
```

最核心的设计决策是**短生命周期的 hook_client 与常驻 daemon 分离**：串口连接必须由 daemon 独占管理（其他程序抢不到端口；且打开串口会让开发板复位，无法承受每次 Hook 调用都来一次）。hook_client 每次只做一次 socket 往返，且必须在 daemon 缺席时安全降级而非崩溃。

设备端是**单任务协作式调度**：全程不占用中断（`tone()` 用 Timer2 是唯一例外），串口字节由硬件 USART 中断搬进环形缓冲，主循环负责解析、看门狗、按钮扫描与动画。**任何一处连续阻塞都不许超过 3 ms**（详见设计文档 5.6，这是不丢行的前提）。

## 通信协议（硬契约，任何改动必须两端同步）

USB 串口 115200 8N1（设备的 D0/D1，经板载 USB 转串口芯片枚举为 COM 口），承载 JSON Lines —— 每条消息以 `\n` 结尾。

> v2.0 用 BLE NUS 承载同一套 JSON Lines。换成串口后 **UUID 与 MTU 的概念整体消失**，但「按 `\n` 重组」的规矩保留 —— 串口读取同样不保证一次读全一条消息。

电脑 → 设备：

```json
{"type":"state","status":"working","msg":"正在分析代码..."}
{"type":"approval_request","request_id":"abc123","tool":"Bash","summary":"rm -rf /tmp/build"}
{"type":"heartbeat","seq":12345}
```

设备 → 电脑：

```json
{"type":"button","request_id":"abc123","action":"approve"}
{"type":"hello","fw":"uno/1.0"}
```

`hello` 是 v3.0 新增的**开机问候**：设备每次复位（上电、按复位键、**电脑端打开串口导致的自动复位**、重新烧录）后发一条，daemon 收到就补发当前画面。复位不会断开串口，所以没有它就探测不到「设备重跑了一遍 setup()」。

**行长是硬约束**：整行 UTF-8 ≤ **320 字节**（固件 `LINE_MAX`，超长整条丢弃、不会把半截行当消息处理）；电脑端单字段 `summary`/`msg` ≤ **160 字节**、`tool` ≤ **24 字节**，截断必须走 `clamp_bytes()`（按 UTF-8 边界，不切碎汉字）。

**为什么是 320 而不是更小**：`approval_request` 的骨架开销约 66 字节 + request_id(8) + tool(24) + summary(160) = 最坏 258 字节。若上限收到 256，带长工具名（如 `mcp__github__create_issue`）的请求会被整条**静默丢弃** —— 屏幕空白、120 秒后超时拒绝，而电脑端全程不知道。

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
- 状态事件超时只有 1 s，失败静默放弃，**stdout 必须零字节** —— 它挂在每一次工具调用
  的路径上，多耗一秒就是实打实地拖慢 Claude Code；多余输出则会污染 Hook 通道。

`PostToolUse` 的成功/失败判定是**启发式的**：只看 `is_error` 与 `error` 两个字段。
Claude Code 各工具的 `tool_response` 结构并不统一，也没有稳定的通用错误字段，
判不出的按成功处理 —— 宁可不报错，也不要误报红色 ERROR。

**设备可见的中文文案受字库约束**：改了上表里的中文（比如把「思考中」改成「正在思考」），
必须把新字加进 `tools/cn_charset.txt` 并重新生成 `cn_font.h`，否则屏幕上显示的是空心方框。

## 时序与常量

| 常量 | 值 | 位置 |
|---|---|---|
| 心跳间隔 | 1.0 s | daemon 主动发送（同时充当链路探针：写失败即判定掉线） |
| 看门狗超时 | 5000 ms | 设备端 `WATCHDOG_TIMEOUT` |
| 按钮去抖 | 200 ms | 设备端 `DEBOUNCE_MS` |
| 审批超时 | 120 s（可配置） | daemon，超时返回 `deny` |
| 本地 Socket | `127.0.0.1:8765` | `hook_client.py` ↔ daemon |
| 动画刷新 | ~50 ms | 设备端主循环 |
| boot grace | ~2.0 s | daemon：打开串口后等 bootloader 走完，期间 `connected` 为 False |
| 串口波特率 | 115200 | 两端一致 |
| BLE 行上限 | 320 字节 | 设备端 `LINE_MAX`，超长整条丢弃 |
| 单字段上限 | 160 / 24 字节 | daemon `MAX_FIELD_BYTES` / `MAX_TOOL_BYTES` |
| 单次连续阻塞 | ≤ 3 ms | 设备端纪律（见设计文档 5.6） |

## 硬件接线（Arduino UNO R3）

屏幕为 **1.77" ST7735S（128×160）**，`tft.setRotation(1)` 的横屏画布是 **160×128**。

| 外设 | 引脚 | 说明 |
|---|---|---|
| TFT CS | D10 | |
| TFT SDA/MOSI | D11 | 硬件 SPI 固定 |
| TFT SCK | D13 | 硬件 SPI 固定；**板载 LED 随之闪烁，不能当状态灯** |
| TFT A0/DC/RS | D9 | 数据 / 命令切换 |
| TFT RES | D8 | 复位 |
| TFT VCC / LEDA | 3.3V | 背光常亮，不占 GPIO（供电见下） |
| 批准按钮 | D2 | `INPUT_PULLUP`，按下为 LOW |
| 拒绝按钮 | D3 | `INPUT_PULLUP`，按下为 LOW |
| 蜂鸣器 | D4 | 无源，`tone()` 方波驱动（AVR 用 Timer2），**必须非阻塞** |
| 状态 LED | D5 | 链路指示灯（心跳正常时点亮） |
| **USB 数据链路** | **D0/D1** | `Serial` 115200，**禁止接任何外设**；同时是烧录口 |

**UNO 没有 strapping 启动脚** —— v2.0 文档里「避开 GPIO9/8、按住按钮上电会不会进下载模式」那一整段在 UNO 上**不存在**，不要照搬过来。

**两条电气约束（白屏的头号嫌疑）**：
1. **UNO R3 的 3.3V 引脚官方标称仅 50 mA**，带背光的 1.77" 模块常超过它 → 易掉电复位/白屏。点不亮时把模块 VCC 改接 **5V**（仅限模块自带 LDO 的型号）。
2. UNO 是 5V 逻辑，多数带串联电阻的模块可直连；若模块是纯 3.3V 输入且接上后发热，需加电平转换或串 100–470 Ω 电阻。

**完整接线**（每个元件的第二根线接哪、接线顺序、上电前检查清单）见**设计文档 4.7**；上面这张表只列引脚分配，给人快速查阅用。4.7 是接线的唯一权威，改引脚时以它为准。

## 硬性约束

- **`hook_client.py` 的 stdout 必须只输出 Hook JSON**。所有调试信息一律写 stderr。stdout 一旦被日志污染，Hook 解析失败会直接破坏审批链路 —— 这是设计文档 8.1 列为「高影响」的风险。
- **Windows 编码**：Python 文本流在中文 Windows 上默认跟随系统 locale（GBK），一旦写入非 GBK 字符就抛 `UnicodeEncodeError`，会让 Hook 进程整个崩掉。`hook_client.py` 因此显式把 stderr reconfigure 成 UTF-8，并用 `sys.stdout.buffer` 直接写 UTF-8 字节。**新增的电脑端脚本必须沿用这个模式**，不要用裸 `print()` 往 stdout/stderr 写非 ASCII 内容。
- **`request_id` 一一对应**。daemon 为每次审批生成唯一 id 并只接受当前等待中的那个，设备回传时必须原样带回。目的是让迟到按钮（上一次审批超时后才按下）不会误批准下一次请求。
- **降级方向一律是拒绝**。审批超时 → `deny`；daemon 连接失败 / 超时 / 解析失败 → `deny`。任何异常都不得让 Hook 崩溃或放行。
- **按钮按下后设备端立即本地切画面**（批准 → `working`/“已批准”，拒绝 → `idle`/“已拒绝”）并作废 `request_id`，不依赖电脑端回话。daemon 的去重模型是「记账 + dirty」（`_last_state` / `_state_dirty`），重连后由 `resend_current()` 补发当前画面：审批进行中重发审批卡（**沿用原 request_id**），否则重发最后一条 state。
- **LOST 恢复语义**：恢复到最后有效状态；若那是 `needs_you` 则恢复为 `idle`（审批请求可能已超时失效，不显示过期卡片）。进入 LOST 时清空 `request_id`，失联期间按键不回传。
- **同一时间只处理一个审批请求**（单槽 `current_request_id`，需求 F9）。多会话 FIFO 队列属于可扩展方向，不在第一版。
- **设备端禁用清单**（每一条都会直接吃掉 2 KB RAM / 32 KB Flash 的预算）：`String` 类、ArduinoJson、任何 `malloc`/`new`、`float`/`double` 与 `sin()`/`sqrt()` 等浮点函数、裸字符串字面量（必须用 `F("...")` 放回 Flash）、循环里的 `delay()`。
- **中文显示**：正文用自制 12px 子集字库（`firmware/VibePet_UNO/cn_font.h`，从 U8g2 的文泉驿点阵宋体裁出 **390 字**（95 个 ASCII + 295 个汉字），12×13 点阵、20 字节位图 + 2 字节索引 + 1 字节步进宽，**每个字约 23 字节 Flash**，合计约 9 KB）；标题类大字用 Adafruit_GFX 内置字体放大 2 倍。**未收录的字形画 12×13 空心方框**，不显示乱码也不静默省略。ASCII 与汉字走同一张表、同一套绘制路径。
- **设备端单行缓冲的两条铁律**（都在 `firmware/VibePet_UNO/` 里，改代码前先读那里的注释）：
  ① 一条消息处理完只摘 `ready` 旗，**绝不清空行缓冲的长度** —— 处理一条消息可能重绘整屏几十毫秒，这期间串口会收进下一条消息的开头，清掉就等于把它截断丢弃（`vibepet_proto.h` 的 `vpLineConsume`）；
  ② 派发函数里**先把字段全部取进局部变量，最后才调用会重绘屏幕的函数**（如 `setState`）—— 因为渲染循环里会 `pumpSerial()`，新字节就写在正在解析的那个缓冲上。
- **改了协议解析（`vibepet_proto.h`）必须跑离线测试**（见「常用命令」）。这一段出错的表现是「屏幕上什么都没有」，真机上极难定位，别跳过。
- **第一版仅 USB 供电**，不支持电池（设备必须连电脑才有数据）。

## 工具链与烧录配置

| 项目 | 选型 |
|---|---|
| 主控 | **Arduino UNO R3**（ATmega328P，16 MHz，32 KB Flash 可用 32256 B / 2 KB SRAM） |
| 嵌入式环境 | Arduino CLI 1.5.1（D 盘 Arduino IDE 自带）或 Arduino IDE |
| AVR 核心 | **`arduino:avr` 1.8.8**（开发板选 `Arduino Uno`，FQBN `arduino:avr:uno`） |
| 图形库 | **Adafruit GFX Library 1.12.6 + Adafruit ST7735 and ST7789 Library 1.11.0**（库管理器安装，另带 BusIO 1.17.4） |
| 中文字库 | 自制子集（`cn_font.h` **随仓库提交**，日常编译不需要 Python）；生成工具 `tools/gen_cn_font.py` 从本机已装的 U8g2 里裁 |
| 电脑端 | Python ≥ 3.9 + **pyserial** 3.5 + asyncio（不再需要 bleak） |

**编译参数不能少**：`--build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256"`（理由见「常用命令」的注释）。

**主频必须与实物一致**：`arduino:avr:uno` 假定 16 MHz。若板子实际是 8 MHz，必须改用 `arduino:avr:pro:cpu=8MHzatmega328`，否则串口波特率偏一半、**每个字节都是乱码**。判断方法：烧录 `test-firmware/uno_link` 看串口输出是否可读。

**烧录前必须先停掉 daemon**，否则串口被占用，上传会失败（daemon 会报「端口被占用」）。同理，调试设备时不要同时开着串口监视器。

**打开串口会让开发板复位**（USB 转串口芯片拉 DTR），bootloader 跑约 2 秒 —— 这不是故障，daemon 的 boot grace 就是为它准备的。

## 调试顺序

设计文档 8.3 规定自底向上分步验证，**每步通过后再进入下一步**，不要在链路未验证时联调：

TFT 点亮（先跑 `test-firmware/tfttest_uno` 例程）→ 串口自检（`test-firmware/uno_link`，115200 不乱码）→ 单行 JSON 能改画面 → 中文能显示（含折行与未收录字）→ 按钮能回传 → 看门狗/LOST 恢复 → daemon 独立测试 → hook_client 命令行测试 → 接入 Claude Code Hook 端到端。

**屏幕点亮之前不要写任何应用代码** —— v2.0 就是在这块屏上卡住的（背光亮、无画面），换库换脚都没解决。先按设计文档 4.7.2 的供电方案 A/B 与初始化序列逐个排除。
