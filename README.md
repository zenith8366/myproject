# 嵌入式自制项目合集

> 一个存放自制嵌入式小项目与相关练题材料的仓库：Arduino / STM32 为主，另收算法练题。

![status](https://img.shields.io/badge/status-持续更新中-brightgreen)
![platform](https://img.shields.io/badge/platform-Arduino%20%7C%20STM32%20%7C%20Python-blue)
![license](https://img.shields.io/badge/license-MIT-yellow)

仓库按**平台分大目录**：`arduino/`、`stm32s/`、`algorithm/`，每个项目放在对应大目录下，
包含源码、文档、接线说明和踩坑记录。一方面方便自己回顾，另一方面也希望能给同好一些参考。

## 目录

- [仓库定位](#仓库定位)
- [项目列表](#项目列表)
- [仓库结构](#仓库结构)
- [开发环境](#开发环境)
- [使用方式](#使用方式)
- [项目组织约定](#项目组织约定)
- [贡献](#贡献)
- [许可证](#许可证)

## 仓库定位

这是一个**个人嵌入式项目合集**，不是某个单一产品的代码库。它同时承担三个作用：

1. **存档**：把自己做过的板子、玩过的模块、写过的固件集中放好，避免散落各处最后找不到；
2. **复盘**：记录每个项目当时的设计思路、踩过的坑、以及为什么最终这样实现；
3. **分享**：如果某个项目刚好能帮到别人，那就更好了。

内容以**小而完整**为主——单个项目通常在一周内能做完，元件成本不高，适合周末动手。不做大而全的框架，也不追求工业级可靠性，重点是把一个想法从零跑通，并把过程写清楚。

## 项目列表

### arduino/ —— Arduino / AVR 平台

#### VibePet —— AI 编程助手物理状态显示与审批终端

把 AI 编程助手（Claude Code）的工作状态搬到桌面上：设备用一根 USB 线连接电脑，
在 1.77 英寸屏幕上实时显示 AI 的运行状态；当 AI 需要批准敏感操作时，设备响一声、
弹出审批卡，按一下物理按钮就能批准或拒绝——不用切窗口，不用敲键盘。

| 项目 | 说明 |
|---|---|
| 平台 | Arduino UNO R3（v3.0 起改为 USB 有线；v2.0 的 BLE 无线方案已放弃） |
| 外设 | 1.77" ST7735S TFT（128×160）、双微动按钮、无源蜂鸣器、状态 LED |
| 电脑端 | Python 3.9+，仅依赖 `pyserial` |
| 目录 | [`arduino/vibepet/`](./arduino/vibepet) |
| 状态 | 固件已烧录真机、正在联调；电脑端测试 44 例、协议内核离线测试 55 例全部通过 |
| 详细说明 | [arduino/vibepet/README.md](./arduino/vibepet/README.md) |

屏幕上的六种状态：`IDLE`（空闲）、`WORKING`（工作中）、`APPROVE?`（等待审批）、`DONE`（完成）、`ERROR`（出错）、`LOST`（连接断开，设备自行判定）。

值得注意的是，test-firmwave目录下对于vibepet所需硬件的测试文件也比较有意思，现列出各个测试项目的内容：

- button——最简单的按键控制LED开关
- esp32wifi——之前vibepet的废案（太菜了没能实现无线传输信息），手机控制LED开关
- genshin——测试无源蜂鸣器，演奏原神主题曲，hbc都说好
- tfttest_esp32c3/uno——聘请AI从网上搜索各大主流AI的LOGO显示（试了ChatGPT，DeepSeek，Grok的网页版，结果Grok做出来最终版效果）

### stm32s/ —— STM32 平台

#### STM32 计算器（Comsen 实验室硬件组招新题）

基于 STM32F103C8T6 的实际可用计算器：1602 字符屏 + 两片 TTP229 共 30 个触摸按键。
保留示例工程的 FreeRTOS 五任务架构，按"同名替换"规则自行实现了应用层、按键滤波与
表达式求值器三个模块。支持 USB 串口显示、触摸按键、四则运算、括号与函数、复数运算、
编辑光标、MODE 设置菜单等功能。

| 项目 | 说明 |
|---|---|
| 平台 | STM32F103C8T6（Cortex-M3，64 KiB Flash） |
| 外设 | 1602 LCD（2 行 × 16 字符）、TTP229 触摸键 ×30、USB CDC |
| 目录 | [`stm32s/Comsen_recruitment_hardware-1.0.0/`](./stm32s/Comsen_recruitment_hardware-1.0.0) |
| 状态 | 功能开发完成、已上板整机验收；提交材料整理中 |
| 详细说明 | [mycalc/README.md](./stm32s/Comsen_recruitment_hardware-1.0.0/mycalc/README.md)（工程说明与编译方法） |

#### demo —— STM32 环境验证小工程

早期验证 CubeMX + CMake 工具链与"编译 → 烧录 → 点亮"闭环的最小工程（点灯级别），
保留作参考。

### algorithm/ —— 算法练题

ComSen 算法组招新题（task1~4，含 OpenCV、PyTorch 练题）的题目素材与自整理的学习指南，
按题号整理在 `problem/` 下。

*更多项目正在路上……*

## 仓库结构

```text
myproject/
├── README.md                  # 本文件
├── LICENSE                    # MIT 许可证
│
├── arduino/                   # Arduino / AVR 平台项目
│   └── vibepet/               # AI 编程助手物理状态显示与审批终端（v3.0）
│       ├── firmware/          # UNO 固件
│       ├── pc/                # 电脑端 Python 程序
│       ├── tools/             # 字库生成、串口探针等
│       └── README.md          # 项目详细说明
│
├── stm32s/                    # STM32 平台项目
│   ├── demo/                  # 工具链验证小工程
│   └── Comsen_recruitment_hardware-1.0.0/   # 硬件组招新 · STM32 计算器
│       ├── mycalc/            # 计算器工程本体（含工程 README）
│       ├── 学习知识/          # 学习文档、测试清单
│       └── README.md          # 招新题目原文
│
└── algorithm/                 # 算法组招新题材料与学习指南
    ├── problem/               # 题目素材（按 task 分目录）
    ├── LEARNING_GUIDE.md      # 学习指南
    └── README.md              # 题目原文（task1~4）
```

## 开发环境

不同项目用的工具链不一样，常见的有：

| 平台 | 工具链 | 备注 |
|---|---|---|
| Arduino / AVR | Arduino IDE / arduino-cli | VibePet 使用；固件里用 Adafruit GFX + ST7735 库 |
| STM32 | CMake + Ninja + arm-none-eabi-gcc（STM32CubeCLT） | 计算器与 demo；可用 CubeMX 打开 `.ioc` 查看配置 |
| PC 端 / 算法 | Python 3 + PyTorch / OpenCV | 按各项目 README 的依赖说明安装 |

每个项目的 README 会写明它自己需要哪些工具链、哪些库、以及具体的版本要求。**不要假设所有项目环境一致**。

## 使用方式

每个项目相互独立，进入对应目录查看它自己的 README。一般流程是：

1. 阅读该项目的 README，确认硬件清单和依赖；
2. 按说明接线、配置工具链、烧录固件；
3. 如有电脑端/上位机程序，安装对应依赖并运行；
4. 遇到问题先看该项目的「常见问题」一节。

## 项目组织约定

为了让仓库保持整洁，新增项目时建议遵循以下约定：

**目录命名**

- **顶层按平台分大目录**（如 `arduino/`、`stm32s/`），具体项目放在大目录下，一个项目一个目录；
- 目录名避免空格和中文，避免与已有项目重名；

**每个项目目录至少包含**

```text
项目名/
├── README.md          # 必需：硬件清单、接线、依赖、烧录、用法、常见问题
├── firmware/ 或 src/  # 固件 / 源码
├── docs/              # 可选：原理图、接线图、设计文档、照片
└── LICENSE            # 可选：如与仓库整体许可不同，单独声明
```

**README 建议包含**

- 一句话说明这个项目是做什么的；
- 硬件清单（型号 + 大致价格）；
- 接线说明或接线图；
- 依赖的第三方库及版本；
- 烧录 / 编译 / 运行的步骤；
- 当前状态（是否经过真实硬件验证）；
- 已知问题和踩坑记录。

**代码风格**

- 固件代码尽量保持可读，关键逻辑写注释；
- 涉及引脚定义、常量、可调参数时，集中放在文件顶部或单独的配置区；
- 不提交编译产物（`build/`、`.pio/`、`*.bin`、`*.hex` 等），用 `.gitignore` 排除。

## 贡献

这个仓库主要是个人项目存档，但非常欢迎交流：

- **提 Issue**：如果你发现某个项目的代码有问题、文档有误，或者接线说明不清楚，欢迎指出；
- **提 PR**：修复 bug、改进文档都很欢迎。如果是较大的功能改动，建议先开 Issue 讨论一下；
- **自己加项目**：如果你想把自己的项目也放进来（虽然更推荐 fork 后自己维护），请遵循上面的[项目组织约定](#项目组织约定)。

## 许可证

除非项目内另有说明，本仓库所有项目均采用 **MIT License**。你可以自由使用、修改、分发，保留版权声明即可。

各项目如引用了第三方库，其许可归属原作者，使用时请遵守对应库的许可条款。

## 联系

- GitHub: [@zenith8366](https://github.com/zenith8366)
- Email: lyh351608807@outlook.com or ericalaplce8@gmail.com
- 有问题优先提 Issue，方便其他人也能看到答案。

---

<p align="center">
  如果这里某个项目对你有帮助，欢迎点个 Star ⭐
</p>
