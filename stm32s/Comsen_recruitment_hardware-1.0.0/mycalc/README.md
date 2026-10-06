# STM32 计算器工程说明

> Comsen 实验室硬件组招新任务 · 完整可编译工程。
> 本工程由招新仓库中的 `example/` 示例改名而来（本目录原名 `example`），保留示例的
> FreeRTOS 五任务架构，按根 README 的规则做**同名替换**：三个模块改为自己实现，
> 两个驱动继续使用提供的静态库。

## 一、完成了哪些内容与模块

| 招新任务 | 完成情况 |
| --- | --- |
| 1. 硬件基础 | 板卡焊接完成、供电与连接检查通过；示例与自研固件均能正常点亮 |
| 2. 串口与显示 | USB 虚拟串口接收电脑文本并显示在屏幕（`PC:` + 13 字符滚动窗口），同时回显到电脑；拔插 USB 后功能自动恢复 |
| 3. 触摸按键 | 自行实现按键滤波（30 ms 三次确认去抖 + 按住只触发一次）与 30 键映射；含按下/释放边沿判定 |
| 4. 基本计算器 | 数字/四则运算输入、AC 全清、DEL 删除、BACK 撤销（按式子回滚，深度 8 层）、`=` 求值 + 结果回填连续运算 |
| 5. 函数扩展 | 自写递归下降求值器：括号嵌套、sin/cos/tan（DEG/RAD 感知）、log/ln/sqrt、pi/e、E 科学计数、隐式乘法（如 `2pi`、`3sin(30)`） |
| 6. 自选提高 | 复数运算（CMPLX 模式：`i`、极坐标 `a@θ`、复数四则）、显示精度切换（FMT：2/4/6 位小数）、SHIFT 副功能、编辑光标、MODE 设置菜单 |

### 按键布局（6 行 × 5 列，共 30 键）

|  | 列 1 | 列 2 | 列 3 | 列 4 | 列 5 |
| --- | --- | --- | --- | --- | --- |
| 行 1 | SHIFT | BACK 撤销 | MODE 设置 | ↑ | OK 确认 |
| 行 2 | `(` | `)` | ← | ↓ | → |
| 行 3 | `7`（π） | `8`（∠） | `9`（i） | DEL | AC |
| 行 4 | `4`（e） | `5`（log） | `6`（ln） | `*` | `÷`（√） |
| 行 5 | `1`（sin） | `2`（cos） | `3`（tan） | `+` | `-` |
| 行 6 | `0` | `.` | x10^x（E） | FMT | EXE |

- 括号内为 SHIFT 副功能：按 SHIFT 后由下一键输入（屏幕提示 `SHIFT ACTIVE`）；
  其中 `∠`（`8`）和 `i`（`9`）仅 CMPLX 模式生效。
- 编辑光标：←/→ 左右移一格，↑/↓ 在两行（每行 16 字符）间跳行；输入的字符插入到
  光标处，DEL 删除光标左侧字符。
- FMT 循环切换显示精度；MODE 打开设置菜单（按 `1` 切换 COMP/CMPLX、按 `2` 切换 DEG/RAD，
  按 OK 确认退出；菜单内其它键无反应）。
- `x10^x` 键输入科学计数记号 `E`（如 `1.5E3`）。

## 二、自己实现 / 使用提供的库（同名替换）

### 自己实现（同名同签名替换原库，调用方代码零改动）

| 自己实现的文件 | 替换的库 | 内容 |
| --- | --- | --- |
| `User/calculator_app.c` | libcalculator_app.a | 五个任务体、按键映射与事件队列、输入状态机、撤销栈、显示快照与编辑光标、USB 收发应用逻辑 |
| `User/touch_filter.c` | libtouch_filter.a | 触摸滤波：每 10 ms 一次的稳定判定 + 单键锁定（按住只触发一次） |
| `User/calculator_engine.c` | libcalculator_engine.a | 表达式求值器：递归下降（表达式 → 项 → 因子），含括号、函数、复数、隐式乘法 |

> 替换后已从 `CMakeLists.txt` 的链接列表移除对应 `.a`，不同时保留同名实现与原库。

### 继续使用提供的库（如实说明）

| 库 | 用途 | 说明 |
| --- | --- | --- |
| liblcd1602.a | 1602 字符屏显示驱动 | GPIO 时序驱动未自行重写 |
| libttp229.a | 30 键位图原始读取 | GPIO 模拟两线时序未自行重写 |
| FreeRTOS / ST USB 协议栈（`Middlewares/`） | 任务调度 / USB CDC | 示例自带，保留使用 |

### 移除未使用的库

- libtouch_model.a —— 触摸组合识别模型，本方案未使用，自 D5 起从链接列表移除。

## 三、源码清单（相对原始示例的改动）

新增（本工程自己的代码）：

- `User/calculator_app.c` —— 应用与界面（主交付物）
- `User/calculator_engine.c` —— 表达式求值器
- `User/touch_filter.c` —— 按键滤波
- `build.sh` / `build.bat` —— 本机一键编译脚本（Windows；原示例的 Linux 版 `build.sh` 已删除替换）

修改：

- `Core/Src/main.c` —— USER CODE 内解除 USB 屏蔽宏、新增 `MX_USB_DEVICE_Init()` 调用
  （示例默认不启动 USB，任务 2 需要）
- `USB_DEVICE/App/usbd_cdc_if.c` / `.h` —— 新增 128 字节接收环形缓冲与
  `CDC_RxTake()` / `CDC_RxOverflow()` 接口
- `CMakeLists.txt` —— 加入 `User/` 三个源文件；移除被替换的 4 个库；链接数学库 `m`；
  Debug 追加 `-Og`（默认 `-O0` 镜像会超出 64 KiB Flash）

未改动：HAL/CMSIS/FreeRTOS/USB 协议栈源码、CubeMX 生成代码的非 USER CODE 区域、`software.ioc`。

## 四、如何编译

环境：`arm-none-eabi-gcc`（Arm GNU Toolchain）在 PATH，CMake + Ninja。
本机一键脚本（内部把 STM32CubeCLT 的 cmake/ninja 加进 PATH）：

```bash
# Git Bash
./build.sh
```

或直接双击 `build.bat`。等价的通用命令（任何平台，cmake 与 ninja 已在 PATH 时）：

```bash
cmake -S . -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug
```

产物在 `build/Debug/`：`software.elf` / `software.hex` / `software.bin`。

烧录：STM32CubeProgrammer 或 OpenOCD + ST-Link；BIN 写入内部 Flash 的起始地址为 `0x08000000`。
用 CubeMX 打开 `software.ioc` 可查看引脚、时钟与外设配置。

## 五、资源占用（2026-10-06，Arm GNU Toolchain 15.3.1）

| 配置 | FLASH | RAM |
| --- | --- | --- |
| Debug（-Og） | 43628 B / 64 KB（66.6%） | 14256 B / 20 KB（69.6%） |
| Release | 37752 B / 64 KB（57.6%） | 14240 B / 20 KB（69.5%） |

## 六、已知问题与说明

- **测试状态**：最新固件已烧录实物板整机验收通过，覆盖：串口收发 / 触摸按键 / 四则运算与
  错误处理 / 编辑键（AC、BACK、DEL）/ SHIFT 副功能 / 函数与括号 / 复数 / 编辑光标 /
  长时间运行与连续乱按的鲁棒性。
- **功能边界**（如实列出）：
  - sin/cos/tan 在 CMPLX 模式下不支持复数参数；
  - SHIFT + `*` 的幂运算（x^y）未实现；SHIFT + `8`/`9`（∠、i）仅 CMPLX 模式生效；
  - 结果超出 ±1e9 或无效运算显示 `Error`；小数位数由 FMT 设定（2/4/6）；
  - COMP/CMPLX、DEG/RAD、FMT 三项设置掉电不保存（重启回默认值，无持久化存储设计）。
- **硬件使用提醒**：板背面对手部接近敏感、可能误触，建议用纸壳等非导电支撑物垫高使用
  （与官方 README 的提醒一致）。
- **Flash 余量**：Debug 构建已占 66.6%（默认 `-O0` 会超限，故 Debug 使用 `-Og`），
  后续继续加功能需留意空间。
