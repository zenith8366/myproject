#!/usr/bin/env bash
# ============================================================================
# 一键编译脚本（Git Bash / VS Code 终端：./build.sh）
#   - 自动把 cmake / ninja 加进 PATH（STM32CubeCLT bundle）
#   - build 目录不存在时自动配置（Ninja + Debug）
#   - 产物：build/Debug/software.hex / .bin / .elf
# ============================================================================
set -e

# 切到本脚本所在目录（不管从哪里调用都能工作）
cd "$(dirname "$0")"

# cmake / ninja 不在系统 PATH，指向 STM32CubeCLT 的 bundle
export PATH="/c/Users/lyh35/AppData/Local/stm32cube/bundles/cmake/4.3.1+st.1/bin:/c/Users/lyh35/AppData/Local/stm32cube/bundles/ninja/1.13.2+st.1/bin:$PATH"

# 首次（或 build 目录被删）时自动配置
if [ ! -f build/Debug/CMakeCache.txt ]; then
  echo "==> 首次编译，正在配置工程..."
  cmake -S . -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
fi

# 编译
cmake --build build/Debug

echo ""
echo "==> 编译完成: $(pwd)/build/Debug/software.hex"
