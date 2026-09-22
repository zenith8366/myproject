# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库性质

这是 Comsen 实验室硬件组**招新题目**的分发仓库，不是产品代码库。核心事实：

- `example/` 里能看到的只是 STM32CubeMX 生成的骨架（初始化、任务创建、GPIO/TIM 配置）。
- **计算器、LCD、触摸这些模块的实现全部以预编译静态库形式提供**，位于 `example/lib/*.a`，源码不在此仓库。不要试图从 `Core/Src/main.c` 反推计算器行为——那里没有。
- 参赛者的任务是逐模块替换：自己实现某个模块后，从 `example/CMakeLists.txt` 的链接列表中删掉对应 `.a`，加入自己的 `.c`。**不要同时保留同名实现与原库**。
- 仓库文档与代码注释均使用中文。

本目录尚未被 git 跟踪；git 仓库根是 `E:/myproject`（个人嵌入式项目合集）。在此处的 commit 会落到那个合集仓库里，操作前先确认范围。

## 构建

**上游 README 描述的 `./build.sh` + `toolchain/env.sh` 流程在本副本中不可用**——`setup.sh` 和 `toolchain/` 目录并不存在（那是 Linux/WSL 发布包的一部分）。在 Windows 上直接走 CMake + Ninja（已实测可用）：

```bash
# cmake/ninja 不在系统 PATH，用 STM32CubeCLT 的 bundle；arm-none-eabi-gcc 已在 PATH 中
export PATH="/c/Users/lyh35/AppData/Local/stm32cube/bundles/cmake/4.3.1+st.1/bin:/c/Users/lyh35/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin:$PATH"

cmake -S example -B example/build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build example/build/Debug
```

- 编译器：`D:\Arm\GNU Toolchain mingw-w64-i686-arm-none-eabi\bin\arm-none-eabi-gcc.exe`（Arm GNU Toolchain 15.3.1，已在 PATH）。`lib/*.a` 由 Arm GNU Toolchain 15.2 构建，ABI 兼容。
- 产物：`example/build/<Config>/software.elf`、`.hex`、`.bin`、`.map`（hex/bin 由 `CMakeLists.txt` 的 POST_BUILD 生成）。
- 实测占用：Debug FLASH 44240 B (67.5%) / RAM 13184 B (64.4%)；Release FLASH 39852 B (60.8%)。**Flash 只有 64 KiB，余量不多。**
- 用 CubeMX 打开 `example/software.ioc` 可查看或重新生成配置。

## 无自动化测试

仓库没有测试框架，也没有能在 PC 上跑的仿真。改动的验证手段只有两条：编译通过（关注链接输出的 FLASH/RAM 占用），以及烧录到板上观察。不要在无法上板时声称功能已验证。

烧录用 STM32CubeProgrammer（`D:\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin`）或 OpenOCD（`D:\xpack-openocd-0.12.0-7\bin`，配 ST-Link）。BIN 写入时起始地址为 `0x08000000`（README 已述）。`E:/myproject/stm32s/demo/.vscode/launch.json` 有一份 cortex-debug + OpenOCD（`interface/stlink.cfg` + `target/stm32f1x.cfg`）的可用配置范例。仓库本身不带烧录脚本。

## 架构

MCU 为 STM32F103C8T6（Cortex-M3，64 KiB Flash / 20 KiB RAM），HSE 8 MHz × 9 = 72 MHz。

### 时基分工

- **SysTick 归 FreeRTOS**（`configTICK_RATE_HZ = 1000`），HAL 时基因此改用 **TIM4**，实现在 `Core/Src/stm32f1xx_hal_timebase_tim.c`，中断里由 `main.c` 的 `HAL_TIM_PeriodElapsedCallback` 调 `HAL_IncTick()`。改时基会同时影响 RTOS 与 `HAL_Delay`。
- **TIM3 预分频 71 → 1 MHz 自由计数**，是 lcd1602/ttp229 两个驱动库的计时基准。必须在调用驱动前 `HAL_TIM_Base_Start(&htim3)`，`main.c` 已在 `USER CODE 2` 段完成。驱动通过外部符号 `htim3`（全局非 static）引用它，改名或改成静态会导致链接失败。

### FreeRTOS

CMSIS-RTOS v2 接口，**全部静态分配**：5 个任务的栈和 TCB 都是 `main.c` 里的静态数组，队列由 `calculator_app_init()` 在 `RTOS_QUEUES` 段以静态方式创建。`configTOTAL_HEAP_SIZE` 仅 1024 字节（heap_4），`configUSE_MALLOC_FAILED_HOOK = 1`，其 hook 直接 `NVIC_SystemReset()`——内存不足表现为复位而非报错。

`main.c` 里每个任务函数体都只是转发到库函数：`KeyTask → calculator_key_task`（osPriorityHigh）、`defaultTask → calculator_heartbeat_task`、`lcdTask → calculator_lcd_task`、`controllerTask → calculator_controller_task`、`calculatorTask → calculator_compute_task`。任务的真实逻辑在 `libcalculator_app.a` 内。

数据流：`ttp229_read_physical()` 原始 30 位位图 → `touch_filter_update()`（约定每 10 ms 调一次）→ `touch_model_classify()` → 控制任务 → 计算队列 → `calculator_evaluate()` → 显示队列 → LCD 任务。

### USB CDC 是被屏蔽的

`.ioc` 里配了 USB Device CDC（PA11/PA12，48 MHz 时钟源），但 `main.c` 用 `#define MX_USB_DEVICE_Init() ((void)0)` 把初始化架空了——**示例默认不启动 USB**，招新第 2 项任务（串口显示）需要自行解除。USB 配置保留给单独的硬件自检镜像使用，改动时不要破坏它。

`USB_DEVICE/App/usbd_cdc_if.c` 里有一处项目自己的改动：`CDC_Receive_FS` 中做了大小写不敏感的 `"EXPORT"` 匹配，置位 volatile 标志，并提供 `CDC_TakeExportRequest_FS()` 供上层取走（已在 `usbd_cdc_if.h` 声明）。本镜像中没有任何调用方。

### 静态库接口与依赖

头文件在 `Core/Inc/`，库在 `lib/`。各库的未定义符号（`arm-none-eabi-nm libXXX.a | grep ' U '` 可复查）暴露了它们的真实依赖：

| 库 | 依赖 | 裸机可复用 |
| --- | --- | --- |
| `libcalculator_engine.a` | libm（`powf`/`atan2f`/`hypotf`/`expf`…） | 是，需链接 `m` |
| `liblcd1602.a` | `htim3`、`HAL_Delay` | 需保持示例引脚与 HAL 毫秒时基 |
| `libttp229.a` | `htim3` | 需保持示例引脚 |
| `libtouch_filter.a` | `memset` | 是，按 10 ms 周期调用 |
| `libtouch_model.a` | `memcpy`/`memset` | 是，内含默认模型 |
| `libcalculator_app.a` | FreeRTOS（`vTaskDelay`、`xQueueGenericCreateStatic`…）、上述全部库、`HAL_GPIO_TogglePin` | 否（任务逻辑，裸机需自写） |

`lcd1602_write_lines`/`lcd1602_write_frame` 接收的是**定长 16 字符**的行缓冲（两行 ×16 字符的字符屏）。`touch_filter_t`、`touch_model_t` 的结构体定义是公开的，可直接在栈上或静态分配。

引脚分配只能从 `example/software.ioc` 和 `main.c` 的 `MX_GPIO_Init()` 读到（PB6、PB8、PB10–PB15 推挽输出，PB7、PB9 输入，PC13 输出）；**具体哪个引脚接 LCD、哪个接 TTP229 的信息只存在于库内部和实际板子上**，不要凭猜测断言。TTP229 用的是 GPIO 模拟两线时序，不是硬件 I²C 外设。

## 重新生成 CubeMX 时不要丢的东西

`software.ioc` 是 CubeMX 的输入，重新生成会覆盖 `USER CODE` 区域之外的内容。以下是本项目在 CubeMX 产物上做过的调整：

- `Core/Inc/` 中的 `calculator_*.h`、`lcd1602.h`、`ttp229.h`、`touch_*.h` 和整个 `lib/` 不是 CubeMX 生成的，必须保留。
- `CMakeLists.txt`：链接 6 个 `.a`、POST_BUILD 生成 HEX/BIN、Debug 追加 `-Og`（`gcc-arm-none-eabi.cmake` 里默认的 `-O0` 会让镜像**超过 64 KiB 链接失败**，这个覆盖不能删）。
- `stm32f103c8tx_flash.ld`：`_Min_Heap_Size = 0x0`（关掉 newlib 堆，RAM 留给 FreeRTOS 静态栈），`_Min_Stack_Size = 0x400`。
- `cmake/gcc-arm-none-eabi.cmake`：编译选项为 `-Wall -Wextra -Wpedantic`，各配置的 flags 用 `CACHE ... FORCE` 设置；链接参数含 `--specs=nano.specs`、`-Wl,--gc-sections`、`--print-memory-usage`。
- `Core/Src/freertos.c` 中只有 `vApplicationMallocFailedHook`，没有业务代码。
