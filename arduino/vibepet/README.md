# VibePet

把 AI 编程助手的工作状态搬到桌面上。

> **当前状态**：本版是 **v3.0（USB 有线版）**，设计与代码都已经落地 ——
> 固件能编译（Flash 30882/32256，约 96%；RAM 1299/2048，约 63%；零警告），
> 电脑端四套测试共 **49 例**、协议内核离线测试 **55 例**全部通过。
> **固件已烧进板子并跑通审批**；最新 Clawd 静态画面的真机观感、
> 长中文折行与缺字框还在确认。
> 进度详见文末的[项目状态](#项目状态)。v2.0 的无线（BLE）路线已放弃（原因见设计文档 1.2）。

---

## 这是什么

用 Claude Code 写代码时，你是不是经常这样：

- 每隔几分钟切回终端，看 AI 到底做完了没有；
- AI 卡在等你确认一条 `rm` 命令上，而你正在看别的窗口，它就一直干等着。

VibePet 是一个放在桌上的小设备。它用一根 USB 线连到你的电脑，把 AI 的工作状态实时显示
在一块 1.77 英寸屏幕上；当 AI 需要批准敏感操作时，设备会响三声短鸣、弹出审批卡，
指示灯同时点亮，你按一下设备
上的按钮就能批准或拒绝——不用切窗口，不用敲键盘。

```
你的电脑                                   桌上的设备
┌──────────────┐                          ┌──────────────┐
│ Claude Code  │  Hook                    │              │
│      ↓       │                          │   APPROVE?   │
│ hook_client  │ ──┐                      │  rm -rf ...  │
└──────────────┘   │  本地 Socket         │              │
                   ↓                      │  [✓]   [✗]   │
              bridge_daemon ──USB 线────→ │              │
                   ↑                      └──────────────┘
                   └────────── 按钮决策 ────────┘
```

设备**靠这根 USB 线供电，也靠它传数据**（所以拔了线它就不工作了）。好处是插上就能用：
不需要配对、不需要装蓝牙驱动、不需要管信号强度。

## 屏幕上的七种状态

| 屏幕显示 | 颜色 | 含义 |
|---|---|---|
| `IDLE` | 白 | 空闲，AI 待命中 |
| `WORKING` | 绿 | AI 正在干活 |
| `APPROVE?` | 黄 | **需要你按按钮**，下方显示待执行的命令 |
| `DONE` | 青 | 这一轮任务完成 |
| `ERROR` | 红 | 工具执行出错，下方显示错误信息 |
| `LOST` | 橙 | 设备收不到电脑的心跳，连接断了 |
| `CHECK PC` | 蓝紫 | **请到电脑答题**：选择题 / 计划确认在电脑上完成 |

屏幕为 **128×160 竖屏**，画面是 Claude Code 的吉祥物 **Clawd**——一只珊瑚橙像素螃蟹，
米白底、经典橘本体、正文放在对话气泡里。所有画面都是**静态**的，只在状态切换时重绘一次；
七种状态通过 Clawd 的表情（方块眼 / 半睁 / 笑眼 / 叉眼 / 闭眼）和头顶道具（感叹号、对勾、
乌云、断线符号、小显示器）区分。正文每行约 9 个汉字，最多 4 行，超长用 `..` 收尾。

`LOST` 是设备自己判断的：电脑端每秒发一次心跳，超过 5 秒收不到就自动切换，
所以设备**不会**卡在某个过期状态上骗你。心跳恢复后设备会自己切回失联前的有效状态
（若原来停在审批卡/交互提醒上则回 `IDLE`），daemon 重连后还会把当前状态重新推一份
（屏幕不用手动重启）。

`IDLE` / `WORKING` / `DONE` 的标题下方会显示一行说明（中文，如「思考中」「Bash 完成」）；
按完审批按钮，屏幕**立刻**变成「已批准」或「已拒绝」，不用等电脑端回话。若失联前
停在审批卡上，恢复后显示 `IDLE` 而不是那张可能已经过期的卡片。

> **中文只收录了约 400 个常用字**（设备只有 32 KB Flash，装不下整套中文字库）。
> 命令摘要里如果出现没收录的字，屏幕会画一个**空心方块**——一眼能看出"少了个字"，
> 而不是显示乱码。想把某个字加进去，见[字库](#想加字怎么加)。

## 准备工作

### 硬件

| 元件 | 型号 | 说明 |
|---|---|---|
| 主控 | Arduino UNO R3（或兼容板） | 约 ¥30~60 |
| 屏幕 | 1.77" ST7735S TFT（128×160，SPI） | 约 ¥15 |
| 按钮 | 6×6 mm 微动开关 ×2 | 批准 / 拒绝 |
| 蜂鸣器 | 无源蜂鸣器（2 脚） | 审批提示音 |
| 指示灯 | 3 mm LED + 220 Ω 电阻 | 失联 / 待审批时点亮 |
| 数据线 | **USB-A/B 方口线（必须支持数据传输）** | 纯充电线不行 |

整机元件成本控制在 ¥100 以内（设计文档 2.3 的成本约束）。

> **买线注意**：UNO 用的是**方口（B 型）USB 口**，不是现在常见的 USB-C 或 Micro-USB。
> 手里只有充电线的话，插上电脑是找不到串口的。

### 接线

先把线接好再烧固件。引脚分配的依据、电气注意事项和完整的上电前检查清单见
[设计文档](VibePet%20——%20AI%20编程助手物理状态显示与审批终端（USB%20有线版）v3.0.md)的 **4.7 完整接线指南**。

| 元件 | 元件引脚 | 接到 |
|---|---|---|
| 屏幕 ST7735S | VCC | 3.3V（点不亮就改 5V，见下） |
| | GND | GND |
| | CS | **D10** |
| | SDA（部分模块标 MOSI） | **D11** |
| | SCK（部分模块标 SCL） | **D13** |
| | A0（部分模块标 DC 或 RS） | **D9** |
| | RES（部分模块标 RST） | **D8** |
| | LED（部分模块标 BL 或 LEDA） | 3.3V（背光常亮） |
| 批准按钮 | 一个脚 | **D2** |
| | 对角的另一个脚 | GND |
| 拒绝按钮 | 一个脚 | **D3** |
| | 对角的另一个脚 | GND |
| 蜂鸣器 | ＋（长脚） | **D4** |
| | － | GND |
| 状态 LED | 长脚（正极） | 先串 220 Ω 电阻，再入 **D5** |
| | 短脚（负极） | GND |

> **丝印对不上就查这里**：`RS` / `A0` / `DC` 是同一根脚（数据/命令切换），接 **D9**；
> `LEDA` / `BL` / `LED` 也是同一根脚（背光正极），接 3.3V。别当成两根线去找。

```text
     Arduino UNO R3              1.77" ST7735S TFT
     ──────────────              ─────────────────
        3V3 ─────────────────────── VCC    （白屏就改 5V，见下）
        GND ─────────────────────── GND
        D10 ─────────────────────── CS
        D11 ─────────────────────── SDA (MOSI)
        D13 ─────────────────────── SCK
         D9 ─────────────────────── A0 (DC/RS)
         D8 ─────────────────────── RES
        3V3 ─────────────────────── LED / LEDA（背光，常亮）

     Arduino UNO R3              其余外设
     ──────────────              ────────
         D4 ─────────────────────── 蜂鸣器 ＋（－ 接 GND）
         D2 ─────────────────────── 批准按钮（另一端接 GND）
         D3 ─────────────────────── 拒绝按钮（另一端接 GND）
         D5 ──── 220Ω ── LED ────── GND

     （D0/D1 接 USB 线到电脑，千万别往上接外设）
```

**D0 和 D1 上什么都不要接** —— 那是设备跟电脑说话的通道，同时也是烧录口。接上任何外设
都会让电脑认不到设备。

**UNO 没有别的"禁区引脚"**：网上关于 ESP32 的「避开 GPIO9」之类的说法在这里不适用。

**接线顺序**（每步都能单独验证，别一次接完再上电）：

1. **先接电源与屏幕**：烧个例程试屏幕（见下文的「先点亮屏幕」）；
2. **接两个按钮**：微动开关的 4 个脚里**同侧两脚是通的**，要跨对角接（一个脚接 IO、对角那个接 GND），按下才会导通；
3. **接蜂鸣器和 LED**：LED 必须串 220 Ω 电阻，直接怼会烧掉。

**接错了会怎样**：

| 症状 | 多半是这个原因 |
|---|---|
| 屏幕完全不亮 | VCC / GND 没接上，或背光的 LED 脚忘了接 3.3V |
| 背光亮、但一片空白，或亮一下又灭 | **供电不足**：UNO 的 3.3V 引脚只能给 50 mA，带背光的模块常常超。把模块 VCC 改接 **5V**（仅限模块自带稳压的型号，多数 1.77" 模块都有） |
| 花屏、偏色、显示区域偏一格 | **不是接线问题**，是屏幕初始化序列不匹配 → 改固件里的 `INITR_BLACKTAB`，依次换 `INITR_GREENTAB` / `INITR_REDTAB` 试 |
| 按按钮没反应 | 微动开关跨错了脚（接成同侧两脚 = 一直处于按下状态），或没接 GND |
| 蜂鸣器只「咔哒」一声 | 装的是**有源**蜂鸣器。本项目按无源驱动，把固件里的 `BUZZER_ACTIVE` 改成 `1` |
| 插上 USB 找不到串口 | 用的是充电线（不是数据线）；或 D0/D1 上接了东西 |
| 屏幕刷新时板上小灯一直闪 | 那是 D13 的板载 LED，它和屏幕的时钟线共用一根脚——**正常现象**，别去"修"它 |

### 电脑

- Python **3.9 或更高**（开发环境实测 3.14.7）
- Claude Code
- 支持平台：Windows / macOS / Linux

## 安装

### 第一步：先点亮屏幕（强烈建议）

**在装任何东西之前**，先用仓库里的例程确认这块屏在你的板子上能亮：

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
"$CLI" compile --fqbn arduino:avr:uno --upload -p COM7 test-firmware/tfttest_uno
```

屏幕开始轮播几个 AI 图标就说明显示链路没问题。这一步能省掉后面一大堆"到底是硬件坏
还是代码错"的排查——设计文档 8.3 也把它列为第一步。

### 第二步：烧录设备固件

固件在 `firmware/VibePet_UNO/VibePet_UNO.ino`，用 Arduino IDE 打开 `firmware/VibePet_UNO/` 文件夹即可。

**1. 安装开发板支持**

Arduino IDE 里：工具 → 开发板 → 开发板管理器，搜索 `Arduino AVR Boards` 安装。
装好后 工具 → 开发板 选 **Arduino Uno**。

**2. 安装两个库**（工具 → 管理库）

| 库 | 用途 |
|---|---|
| **Adafruit GFX Library** | 图形基础库 |
| **Adafruit ST7735 and ST7789 Library** | 屏幕驱动 |

中文字库**不用装**：它已经跟着仓库提交了（`firmware/VibePet_UNO/cn_font.h`），
正常编译完全不需要 Python。

**3. 编译并烧录**

```bash
CLI="/d/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"

# 查看板子占用的串口
"$CLI" board list

# 编译 + 烧录（把 COM7 换成你实际的）
"$CLI" compile --fqbn arduino:avr:uno \
  --build-property "build.extra_flags=-DSERIAL_RX_BUFFER_SIZE=256" \
  --build-path firmware/VibePet_UNO/build \
  --upload -p COM7 firmware/VibePet_UNO
```

| 参数 | 作用 |
|---|---|
| `-p COM7` | 板子占用的串口，用上一条命令查 |
| `-DSERIAL_RX_BUFFER_SIZE=256` | 把串口接收缓冲从默认 64 字节提到 256，极端情况下更不容易丢报文 |
| `--build-path …/build` | **不能省**：不带它时上面那条 `-D` 会静默失效（编译器的缓存机制会命中一份没带参数的旧结果），而 RAM 数字是唯一的判据 |

> **怎么确认参数真的生效了**：看编译输出里的那两行。带参数时应该接近
> `Global variables use 1299 bytes`；如果显示 `1090 bytes`，说明 `-D` 没生效，
> 加回 `--build-path` 再编一次。

### 或者：用 Arduino IDE 的「上传」按钮（更省事）

v3.0 换了主控之后，**Arduino IDE 的上传按钮可以直接用了** —— v2.0 必须走命令行，
因为要带 `--libraries` 参数指向仓库自带的 TFT_eSPI，而 IDE 不会带这个参数。

1. 用 Arduino IDE 打开 `firmware/VibePet_UNO/VibePet_UNO.ino`
   （同目录的 `cn_font.h` 与 `vibepet_proto.h` 会作为标签页一起打开，不用管它们）
2. 工具 → 开发板 → **Arduino Uno**；工具 → 端口 → 选设备所在的 COM 口
3. 库管理器里装好上表那两个库
4. **先停掉 daemon**（串口同一时刻只能被一个程序拿着），点「上传」

> **和命令行的唯一区别**：串口接收缓冲是默认的 64 字节，而不是命令行那条
> `-DSERIAL_RX_BUFFER_SIZE=256` 指定的 256 —— 这个参数在 IDE 里没有对应的设置项。
> **不影响使用**：固件里「任何一处连续阻塞不超过 3 毫秒」的纪律保证了屏幕上最费时的
> 一次绘制最多只会攒下约 29 字节，64 字节够用（还因此多留了 192 字节给栈）。
> 命令行那条 256 只是多一层保险。

> 用 IDE 时别同时开着「串口监视器」—— 它会占住串口，daemon 就连不上了。

> ⚠️ **烧录前先停掉 daemon**，否则串口被占用，上传会失败。同理，别同时开着 Arduino IDE
> 的串口监视器。

烧录成功后屏幕先显示开机画面，随后进入 `IDLE`。

### 第三步：安装电脑端依赖

```bash
pip install pyserial
```

只需要这一个第三方库。

### 第四步：配置 Claude Code Hook

编辑项目目录下的 `.claude/settings.json`，把 VibePet 挂到这些事件上。
配置使用 `${CLAUDE_PROJECT_DIR}` 定位脚本，不需要写死仓库绝对路径：

```json
{
  "hooks": {
    "PreToolUse": [{"matcher":"Bash","hooks":[{"type":"command","shell":"bash","timeout":150,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "PermissionRequest": [{"hooks":[{"type":"command","shell":"bash","timeout":150,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "PostToolUse": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "UserPromptSubmit": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "Stop": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "SessionStart": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "SessionEnd": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "SubagentStop": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}],
    "PreCompact": [{"hooks":[{"type":"command","shell":"bash","timeout":5,"command":"python \"${CLAUDE_PROJECT_DIR}/pc/hook_client.py\""}]}]
  }
}
```

`PreToolUse` 的 `Bash` matcher 让每次 Bash 调用都经过物理审批；无 matcher 的
`PermissionRequest` 则只在 Claude Code 需要权限决定时触发，覆盖非 Bash 工具。
状态事件只上报屏幕状态，不要求按键。

> **并行 Hook 提醒**：如果用户级配置也注册了 `PermissionRequest`（例如 Clawd on Desk），
> 两个匹配 Hook 会同时运行。本项目仓库里的 9 个 Hook 当前是启用的；在确认并行审批行为前，
> 先按 `.claude/README.md` 的说明测试，再决定是否保留这组 Hook。

> **⚠️ 挂上 Hook 之后，一定要先启动 daemon 再使用 Claude Code。**
> daemon 不在时，VibePet 会按安全策略拒绝所有匹配的工具调用——这是有意设计的
> （宁可拒绝，也不放过未经审批的操作），但如果你忘了启动它，会觉得 Claude Code 坏了。

### 在 Codex 中启用

Codex Hook 配置位于仓库内的 `.codex/hooks.json`，因此只会在 Codex 会话的
工作目录设为 VibePet 目录或其子目录时加载；仓库其他目录不会加载这组 Hook。它只在
`PermissionRequest` 时等待实体审批按钮，没有配置 `PreToolUse`，因此普通工具调用不会
每次都弹审批卡。事件由 `pc/codex_hook.py` 转给同一个 daemon；Codex 对 `Stop` 等
生命周期事件要求返回 JSON，所以适配器会输出一个空的 `{}`，不会污染 Claude Code 那条链路。

先启动 daemon，再在 Codex 桌面版以 VibePet 目录作为工作目录打开会话，
运行 `/hooks` 检查并信任当前配置。Codex 会按配置内容记录信任，配置后续有变化时需要
重新审核；daemon 不可达或等待超时会按安全策略拒绝审批。

## 日常使用

### 启动

先插好 USB 线，然后开一个终端窗口，保持它开着：

```bash
python pc/bridge_daemon.py
```

**daemon 会自动找到你的板子**（按 Arduino 的 USB 芯片标识优先匹配），打开串口后
屏幕从 `LOST` 变成 `IDLE`，就可以正常用 Claude Code 了。

想看得更详细（每条收发报文）加 `-v`：

```bash
python pc/bridge_daemon.py -v
```

板子插在别的口上、或者自动找错了，可以指定串口：

```bash
python pc/bridge_daemon.py --serial-port COM7
```

> **打开串口会让板子重启一次**（约 2 秒），这是 USB 转串口芯片的设计使然，不是故障。
> daemon 会等它启动完再把当前画面补发过去，屏幕不会停在开机画面上。

### 审批

当 AI 要做需要你批准的事时——运行命令、修改工作区外的文件等，审批都会出现在设备上：

1. 设备**响三声短鸣**且指示灯点亮，屏幕变成黄色的 `APPROVE?`，下面显示命令或操作摘要；
2. 按**批准按钮** → 屏幕立刻变成「已批准」，AI 继续做；
3. 按**拒绝按钮** → 屏幕立刻变成「已拒绝」，AI 不会做这件事；
4. **120 秒**内没有任何操作 → 自动按「拒绝」处理（可以改，见下文）。

同样的操作会在终端里显示为已批准 / 已拒绝。

> **选择题与计划确认不占用设备审批**：`AskUserQuestion` / `ExitPlanMode` 出现时，
> 设备显示蓝紫色的 `CHECK PC` 和「请到电脑答题」，你仍然在电脑上作答；
> 即使 daemon 没启动，这类交互也会正常放行。

### 停止

在 daemon 窗口按 `Ctrl+C`。

## 常见问题

### 所有 Bash 命令都被拒绝了

**最常见的原因：daemon 没有启动。**

VibePet 的降级策略是「拿不准就拒绝」，daemon 缺席时它无法拿到你的决定，只能拒绝。
开一个窗口跑 `python pc/bridge_daemon.py` 即可。
（配置里挂了 `PermissionRequest` 时，非命令类操作——比如改工作区外的文件——同样会被拒绝。）

如果不是这个原因，看 daemon 窗口的错误信息——它会把失败原因写在 stderr 上。

### 屏幕显示 `LOST`

设备收不到心跳了。依次检查：

1. daemon 还在运行吗？（窗口是不是被关了）
2. USB 线松了吗？（换一个 USB 口试试，有些口供电不足）
3. 设备是不是刚被复位过？（daemon 会自动重连，屏幕会自己切回去）

daemon 会自动重连，恢复后屏幕会自己切回去，不用重启：设备先本地恢复到失联前的
画面，daemon 连上后再把当前状态校准一遍。若失联前停在审批卡上，恢复后显示
`IDLE`（那张卡可能已经过期，避免你按下一个无效的按钮）。

### daemon 说找不到串口 / 打开失败

| 提示 | 原因 |
|---|---|
| 没找到串口 | 线是**充电线**（不传数据）；或驱动没装（兼容板常用 CH340 芯片，需要装一次驱动）；或 D0/D1 上接了外设 |
| 端口被占用 | 有别的程序拿着它：Arduino IDE 的**串口监视器**、另一个 daemon、串口助手。关掉再试 |
| 写入失败 / 掉线 | USB 被拔了，或板子复位了。daemon 会每 2 秒自动重试，插上就好 |

### 屏幕不亮，或者亮一下就灭

按顺序排查（详细说明见设计文档 4.7.2）：

1. **背光有没有亮**？不亮 → VCC / GND / LEDA 没接好；
2. **背光亮但没画面** → 大概率是**供电不足**：UNO 的 3.3V 引脚只能给 50 mA，而带背光的
   模块常常要更多。把模块的 VCC 从 3.3V 改接到 **5V**（仅限模块自带稳压的型号）；
3. **还不行** → 换屏幕初始化序列：把固件里的 `INITR_BLACKTAB` 依次换成
   `INITR_GREENTAB`、`INITR_REDTAB` 再试；
4. 最后才怀疑模块本身坏了（换一块屏试）。

### 想单独试试设备（不经过 Claude Code）

用仓库里的串口探针直接跟设备说话（先停掉 daemon —— 串口只能被一个程序拿着）：

```bash
python tools/serial_probe.py COM7
```

打开后会看到 `>` 提示符，敲这些简写就能演一遍主要状态：

```
/state working 正在分析代码
/state idle 空闲了
/approval 删除缓存                ← 真正的审批请求：屏幕变黄 + 三声短鸣
/approve                          ← 假装按了批准按钮
/deny                             ← 假装按了拒绝按钮
```

注意 `needs_you` 只能由 `approval_request` 触发：发 `/state needs_you` 设备会
当作未知状态忽略。想看审批卡请用 `/approval`。

设备回传的每一行（比如按钮消息）会带时间戳打印出来。这是排查「屏幕没反应」
最快的办法：不经过 Claude Code、不经过 daemon，只剩设备和串口。

> **探针会自动每 2 秒替你发一次心跳**。因为设备 5 秒收不到任何消息就会切成
> 橙色的 `LOST` —— 手工调试时你不可能边想边敲，没这个保活就只能看到 LOST。
> 想亲眼看失联画面就加 `--no-heartbeat`。

### 只想先试试，还没接硬件

不需要设备也能跑通整条审批链路：

```bash
python pc/bridge_daemon.py --no-device
```

这个模式下没有真实串口，按钮事件改由本地 Socket 注入。调试协议逻辑时很有用。

### 想加字怎么加

屏幕显示空心方块 = 那个字不在字库里。加字两步：

1. 把字加进 `tools/cn_charset.txt`（人类可读的清单，直接编辑）；
2. 运行 `python tools/gen_cn_font.py --out firmware/VibePet_UNO/cn_font.h` 重新生成，
   再烧一次固件。

每多一个字大约多占 **23 字节** Flash（20 字节位图 + 2 字节码点 + 1 字节步进宽）。
当前 Flash 只剩约 **1.3 KB**（30882/32256）。按每字 23 字节算，上限约 59 个字；
实际建议按 **50 个左右**规划，留一点编译余量。再往上就要先裁剪字集或功能，
不能像早期那样「加几百个字毫无压力」了。

### 审批超时时间太短 / 太长

```bash
python pc/bridge_daemon.py --timeout 300     # 改成 5 分钟
```

> ⚠️ **调到 130 秒以上时必须连客户端一起改**，否则会像"改了没生效"。
> `hook_client.py` 自己还有一道等待上限（默认 130 秒，比 daemon 的 120 秒多留
> 10 秒，好让「超时拒绝」由 daemon 说了算）；而它由 Claude Code 启动，**不继承
> 你启动 daemon 那个终端的环境**——所以 `--timeout` 只改了 daemon 这一半，
> 超过 130 秒的等待会被客户端抢先掐断。
>
> 两边都读同一个环境变量，把它设成**用户级**环境变量即可同时生效
> （设完要重启 Claude Code）：
>
> ```bash
> setx VIBEPET_TIMEOUT 300        # Windows；macOS / Linux 用 export
> ```

### 想确认电脑端一切正常

冒烟测试都不需要硬件：

```bash
python pc/_smoke_test.py           # 12 项：审批路径 + 交互工具豁免
python pc/_smoke_test_daemon.py    # 14 项：含并发、去重、重连补发、审批卡不被顶掉、端到端
python pc/_smoke_test_state.py     # 14 项：状态映射
python pc/_smoke_test_serial.py    # 9 项：串口传输层（注入假串口，不需要真设备）

# 协议解析内核的离线测试（同样不需要硬件：用电脑上的 g++ 编译固件里的解析代码）
g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o /tmp/proto_test \
  && /tmp/proto_test               # 55 项：转义、中文、嵌套、分片、超长行、字库取行
```

## 项目状态

| 部分 | 状态 |
|---|---|
| 设计文档 v3.0（USB 有线版） | ✅ 已完成 |
| 电脑端审批逻辑（`hook_client.py` + `Bridge`） | ✅ 已实现（传输换代没有改审批逻辑；2026-10-01 扩展了 `PermissionRequest`） |
| 电脑端串口传输（`SerialTransport`） | ✅ 已实现，9 项测试通过（注入假串口，不需要真设备） |
| 协议解析内核（`vibepet_proto.h`） | ✅ 已实现，离线测试 55 例通过 |
| UNO 固件（`firmware/VibePet_UNO/`） | ✅ 七态 Clawd 静态画面已实现（Flash 30882/32256，约 96%；RAM 1299/2048，约 63%）；Bash 批准 / 拒绝与 `PermissionRequest` 批准已实机跑通；最新画面的真机观感、长中文折行、缺字框待确认 |
| 中文字库工具链（`tools/gen_cn_font.py`） | ✅ 已实现（394 字、约 9.1KB），黄金字形自检通过 |
| 硬件链路（UNO + 1.77" 屏） | ✅ 屏幕已验证点亮 |
| Codex 适配（`pc/codex_hook.py` + `.codex/hooks.json`） | ✅ 已实现（复用同一 daemon），待 Codex 实机联调 |
| 外壳 / 3D 打印 | ⬜ 未开始 |

**现在能做的**：跑上面的测试验证全部逻辑；按「第二步」把固件烧进板子；按「日常使用」跑通 Claude Code / Codex 的物理审批。
**还没做的**：最新 Clawd 画面的真机观感、长中文折行与缺字框、设计文档里标「待实测」的性能数字、看门狗 LOST 画面的完整观感、多会话审批队列、外壳。

## 深入了解

| 文档 | 内容 |
|---|---|
| [设计文档](VibePet%20——%20AI%20编程助手物理状态显示与审批终端（USB%20有线版）v3.0.md) | 完整规格：需求、硬件选型、协议、资源预算、字库工具链、开发计划、测试方案、风险分析 |
| [CLAUDE.md](CLAUDE.md) | 面向开发者的架构说明与硬性约束 |

通信协议是纯文本的 JSON Lines（一条消息一行），因此任何能读写串口的程序都能接入，
不限于本项目提供的电脑端程序。协议细节见设计文档 3.3。
