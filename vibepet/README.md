# VibePet

把 AI 编程助手的工作状态搬到桌面上。

---

## 这是什么

用 Claude Code 写代码时，你是不是经常这样：

- 每隔几分钟切回终端，看 AI 到底做完了没有；
- AI 卡在等你确认一条 `rm` 命令上，而你正在看别的窗口，它就一直干等着。

VibePet 是一个放在桌上的小设备。它通过蓝牙连到你的电脑，把 AI 的工作状态实时显示在一块
1.77 英寸屏幕上；当 AI 需要批准敏感操作时，设备会响一声、弹出审批卡，你按一下设备上的
按钮就能批准或拒绝——不用切窗口，不用敲键盘。

```
你的电脑                                   桌上的设备
┌──────────────┐                          ┌──────────────┐
│ Claude Code  │  Hook                    │              │
│      ↓       │                          │   APPROVE?   │
│ hook_client  │ ──┐                      │  rm -rf ...  │
└──────────────┘   │  本地 Socket         │              │
                   ↓                      │  [✓]   [✗]   │
              bridge_daemon ──蓝牙──────→ │              │
                   ↑                      └──────────────┘
                   └────────── 按钮决策 ────────┘
```

## 屏幕上的六种状态

| 屏幕显示 | 颜色 | 含义 |
|---|---|---|
| `IDLE` | 白 | 空闲，AI 待命中 |
| `WORKING` | 绿 | AI 正在干活 |
| `APPROVE?` | 黄 | **需要你按按钮**，下方显示待执行的命令 |
| `DONE` | 青 | 这一轮任务完成 |
| `ERROR` | 红 | 工具执行出错，下方显示错误信息 |
| `LOST` | 橙 | 设备收不到电脑的心跳，连接断了 |

`LOST` 是设备自己判断的：电脑端每秒发一次心跳，超过 5 秒收不到就自动切换，
所以设备**不会**卡在某个过期状态上骗你。心跳恢复后设备会自己切回失联前的画面，
daemon 重连后还会把当前状态重新推一份（屏幕不用手动重启）。

`IDLE` / `WORKING` / `DONE` 的标题下方会显示一行说明（中文，如「思考中」「Bash 完成」）；
按完审批按钮，屏幕**立刻**变成「已批准」或「已拒绝」，不用等电脑端回话。若失联前
停在审批卡上，恢复后显示 `IDLE` 而不是那张可能已经过期的卡片。

## 准备工作

### 硬件

| 元件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-C3 SuperMini | 约 ¥15 |
| 屏幕 | 1.77" ST7735S TFT（128×160，SPI） | 约 ¥15 |
| 按钮 | 6×6 mm 微动开关 ×2 | 批准 / 拒绝 |
| 蜂鸣器 | 无源蜂鸣器（低电平触发） | 审批提示音 |
| 指示灯 | 3 mm LED + 限流电阻 | 蓝牙连接状态 |
| 供电 | USB-C 数据线（**必须支持数据传输**） | 纯充电线不行 |

整机元件成本控制在 ¥100 以内（设计文档 2.3 的成本约束）。接线方式见[设计文档](VibePet%20——%20AI%20编程助手物理状态显示与审批终端（无线%20BLE%20版）v2.0.md)第 4 章。

### 电脑

- Python **3.9 或更高**（开发环境实测 3.14.7）
- 蓝牙 4.0 以上，且**已开启**
- Claude Code
- 支持平台：Windows 10 (build 16299+) / macOS / Linux

## 安装

### 第一步：烧录设备固件

固件在 `firmware/VibePet/VibePet.ino`，用 Arduino IDE 打开 `firmware/VibePet/` 文件夹即可。

**1. 安装 ESP32 开发板支持**

文件 → 首选项 → 「附加开发板管理器网址」填入：

```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

然后 工具 → 开发板 → 开发板管理器，搜索 `esp32` 安装（版本需 ≥ 3.0）。

**2. 安装五个库**（工具 → 管理库）

| 库 | 用途 |
|---|---|
| **NimBLE-Arduino** | BLE 协议栈。比 ESP32 自带 BLE 库省一半内存，务必用这个 |
| **TFT_eSPI** | 屏幕驱动 |
| **ArduinoJson** | JSON 解析 |
| **U8g2** | 字体渲染引擎（下面那个库的依赖） |
| **U8g2_for_TFT_eSPI** | 中文渲染。管理器里搜不到时从 GitHub 装：`Bodmer/U8g2_for_TFT_eSPI` |

装了中文字库（文泉驿点阵宋体，覆盖 GB2312）后，屏幕上的中文摘要是正常汉字，
不再是问号。

**3. 配置 TFT_eSPI**

TFT_eSPI 的引脚和屏幕参数**不在代码里**，而在库自己的 `User_Setup.h` 中。把本项目的
`firmware/TFT_eSPI_User_Setup.h` 内容整体复制过去覆盖它（**覆盖前先备份原文件**）。

库文件位置：`我的文档/Arduino/libraries/TFT_eSPI/User_Setup.h`

> ⚠️ 不同厂商的 1.77" 模块出厂初始化参数不同。烧录后若出现花屏、偏色或显示区域
> 偏移一格，改模板里的 `ST7735_INITB` 那一行，依次换 `ST7735_GREENTAB`、
> `ST7735_BLACKTAB` 等逐个试，直到画面正常。

**4. 开发板设置**（工具菜单）

| 项目 | 值 |
|---|---|
| 开发板 | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| Flash Size | 4MB |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| Upload Speed | 921600 |

**5. 上传**

插上 USB 线（要能传数据的线）点上传。若提示找不到串口，按住板载 BOOT 键再插一次 USB。

烧录成功后屏幕先显示 `VibePet / starting...`，随后进入 `IDLE`——此时设备已在广播，
可以进入第二步了。

> 习惯命令行的话，Arduino IDE 自带 arduino-cli，可直接用（注意路径含空格要加引号）。
> 走命令行时**不必覆盖库的 `User_Setup.h`**，引脚配置改用编译参数注入：
> ```bash
> CLI="D:/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
> "$CLI" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" \
>   --build-property 'compiler.cpp.extra_flags=-DUSER_SETUP_LOADED=1 -DST7735_DRIVER -DTFT_WIDTH=128 -DTFT_HEIGHT=160 -DTFT_CS=7 -DTFT_DC=8 -DTFT_RST=5 -DTFT_MOSI=6 -DTFT_SCLK=4 -DLOAD_GLCD -DSPI_FREQUENCY=27000000 -DST7735_INITB' \
>   firmware/VibePet
> "$CLI" upload --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" -p COM3 firmware/VibePet
> ```
> 上面这条命令已实测通过（占用 Flash 26%，零警告）。中文点阵字库约 200 KB，
> 所以 FQBN 里必须带 `PartitionScheme=huge_app`——用默认分区会放不下。如果你的
> 模块不是 INITB 序列，把末尾的 `-DST7735_INITB` 换成 `-DST7735_GREENTAB` 等再试。

### 第二步：安装电脑端依赖

```bash
pip install bleak
```

只需要这一个第三方库。

### 第三步：配置 Claude Code Hook

编辑你的 Claude Code 配置文件 `.claude/settings.json`，把 VibePet 挂到这些事件上
（把 `<绝对路径>` 换成你实际存放本项目的位置）：

```json
{
  "hooks": {
    "PreToolUse":       [{"matcher": "Bash", "hooks": [{"type": "command", "command": "python <绝对路径>/pc/hook_client.py"}]}],
    "PostToolUse":      [{"hooks": [{"type": "command", "command": "python <绝对路径>/pc/hook_client.py"}]}],
    "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "python <绝对路径>/pc/hook_client.py"}]}],
    "Stop":             [{"hooks": [{"type": "command", "command": "python <绝对路径>/pc/hook_client.py"}]}],
    "SessionStart":     [{"hooks": [{"type": "command", "command": "python <绝对路径>/pc/hook_client.py"}]}],
    "SessionEnd":       [{"hooks": [{"type": "command", "command": "python <绝对路径>/pc/hook_client.py"}]}]
  }
}
```

Windows 路径建议用正斜杠（`E:/myproject/vibepet/pc/hook_client.py`），JSON 里不用转义。

`"matcher": "Bash"` 决定哪些工具需要物理审批。想改成所有工具都审批就用 `"*"`，
想只审批特定命令可以在 `"Bash(...)"` 里写匹配规则。

> **⚠️ 挂上 Hook 之后，一定要先启动 daemon 再使用 Claude Code。**
> daemon 不在时，VibePet 会按安全策略拒绝所有匹配的工具调用——这是有意设计的
> （宁可拒绝，也不放过未经审批的操作），但如果你忘了启动它，会觉得 Claude Code 坏了。

## 日常使用

### 启动

开一个终端窗口，保持它开着：

```bash
python pc/bridge_daemon.py
```

设备连上后，指示灯点亮，屏幕从 `LOST` 变成 `IDLE`，就可以正常用 Claude Code 了。

想看得更详细（每条收发报文）加 `-v`：

```bash
python pc/bridge_daemon.py -v
```

### 审批

当 AI 要执行需要批准的操作时：

1. 设备**响一声**，屏幕变成黄色的 `APPROVE?`，下面显示要执行的命令；
2. 按**批准按钮** → 屏幕立刻变成「已批准」，AI 继续执行；
3. 按**拒绝按钮** → 屏幕立刻变成「已拒绝」，AI 不会执行这条命令；
4. **120 秒**内没有任何操作 → 自动按「拒绝」处理（可以改，见下文）。

同样的命令会在终端里显示为已批准 / 已拒绝。

### 停止

在 daemon 窗口按 `Ctrl+C`。

## 常见问题

### 所有 Bash 命令都被拒绝了

**最常见的原因：daemon 没有启动。**

VibePet 的降级策略是「拿不准就拒绝」，daemon 缺席时它无法拿到你的决定，只能拒绝。
开一个窗口跑 `python pc/bridge_daemon.py` 即可。

如果不是这个原因，看 daemon 窗口的错误信息——它会把失败原因写在 stderr 上。

### 屏幕显示 `LOST`

设备收不到心跳了。依次检查：

1. daemon 还在运行吗？（窗口是不是被关了）
2. 蓝牙还开着吗？
3. 设备是不是断电了 / 超出范围了（有效距离约 10 米）

daemon 会自动重连，恢复后屏幕会自己切回去，不用重启：设备先本地恢复到失联前的
画面，daemon 连上后再把当前状态校准一遍。若失联前停在审批卡上，恢复后显示
`IDLE`（那张卡可能已经过期，避免你按下一个无效的按钮）。

### daemon 找不到设备

- **电脑蓝牙没开**：daemon 会明确提示 `Bluetooth radio is not powered on`；
- **设备没上电**：屏幕应该亮着并显示 `LOST`，不亮就是没通电；
- **设备名不是 `VibePet`**：用 `--device-name 你的名字` 指定。

### 只想先试试，还没焊硬件

不需要设备也能跑通整条审批链路：

```bash
python pc/bridge_daemon.py --no-ble
```

这个模式下没有真实蓝牙，按钮事件改由本地 Socket 注入。调试协议逻辑时很有用。

### 审批超时时间太短 / 太长

```bash
python pc/bridge_daemon.py --timeout 300     # 改成 5 分钟
```

### 想确认电脑端一切正常

三套冒烟测试都不需要硬件和蓝牙：

```bash
python pc/_smoke_test.py           # 5 项：审批路径
python pc/_smoke_test_daemon.py    # 11 项：含并发、去重、重连补发、端到端
python pc/_smoke_test_state.py     # 14 项：状态映射
```

## 项目状态

| 部分 | 状态 |
|---|---|
| 电脑端（桥接守护进程 + Hook 客户端） | ✅ 已实现，30 项测试通过 |
| BLE 协议（NUS + JSON Lines） | ✅ 已定义并实现，两端字段逐一对齐 |
| ESP32 固件 | ✅ 已实现，**编译通过**（Flash 占用 26%，零警告；中文点阵字库在内）—— 但**尚未在真实硬件上验证** |
| 外壳 / 3D 打印 | ⬜ 未开始 |

## 深入了解

| 文档 | 内容 |
|---|---|
| [设计文档](VibePet%20——%20AI%20编程助手物理状态显示与审批终端（无线%20BLE%20版）v2.0.md) | 完整规格：需求、硬件选型、协议、开发计划、测试方案、风险分析 |
| [CLAUDE.md](CLAUDE.md) | 面向开发者的架构说明与硬性约束 |

通信协议用的是标准 Nordic UART Service，数据是 JSON Lines，因此任何支持 BLE 的
平台都能接入，不限于本项目提供的电脑端程序。
