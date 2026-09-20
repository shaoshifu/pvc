#!/usr/bin/env bash
# 本机验证构建：不依赖 Emscripten，用 gcc 把"可移植侧 + 软件光栅后端"编成
# 一个能在 Windows 上跑的可执行文件 —— 这是 P2 阶段的主力验证手段。
#
# 【为什么要 --allow-multiple-definition】
#   本机是 Windows，MinGW 默认链接 kernel32/msvcrt，它们已经提供了
#   Sleep / GetTickCount / Arc / Polygon … 而我们自己的软件光栅后端**故意**
#   也定义了同名符号（想复刻 GDI 语义）。
#   不加这个标志 → 链接期 multiple definition。
#   加了之后链接器取**先出现的**定义；我们的 .o 排在库之前，所以
#   生效的是**我们的软件光栅实现**，正是验证想要的效果。
#   ⚠️ 这个标志只用于本机验证；wasm 构建（emcc）没有这个问题，也不需要它。
set -u
cd "$(dirname "$0")"
CFLAGS="-DPLAT_PORTABLE -O2 -Wall -Wextra -I."
LDFLAGS="-Wl,--allow-multiple-definition"
python_exe="${PYTHON:-C:/Python314/python.exe}"

echo "== 编译网页版后端（本机验证）=="
gcc $CFLAGS -o _web_verify.exe _web_main.c plat_web.c pvz.c -lm $LDFLAGS 2>&1 | head -20
if [ ! -f _web_verify.exe ]; then echo "★ 编译失败"; exit 1; fi
echo "  ✓ _web_verify.exe 已生成"

echo
echo "== 跑 60 帧并导出世界层 =="
./_web_verify.exe assets _web_out.raw 60
echo
echo "世界层应输出 2000x1300（= 逻辑 1000x650 × SS 2）"
ls -l _web_out.raw 2>/dev/null | awk '{print "  ",$5,"字节"}'
echo "  预期:", $((4 + 4 + 4 + 2000*1300*4)), "字节（GLB1 头 12 + 像素）"
