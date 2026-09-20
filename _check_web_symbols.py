#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查"可移植侧"有没有漏实现符号 —— 提前发现 emcc 才会暴露的链接错误。

【为什么需要这个检查】
  2026-09-21 的 CI 里，前面 6 步全过，到 `编译 WASM` 失败。
  根因是 `_wcsicmp` 与 `_wgetenv` 这两个 **MSVCRT 专有函数**没有实现：

    · 本机用 MinGW 链接时，kernel32 / msvcrt 会**自动提供**它们，
      所以 `_build_web_local.sh` 一路绿灯，看起来"可移植侧已经完整"；
    · emcc 到 musl 上没有这些符号，**链接期直接失败**。

  这是"本地过、目标平台失败"的经典形态：
  **本机的系统库把缺口盖住了**。编译告警发现不了（`-c` 只编不链），
  只能靠检查**链接期**符号。

【做法】
  ① 把 pvz.c 与 plat_web.c 各编成目标文件（只编不链，所以本机也不会暴露缺口）
  ② 用 nm 取出两边的「未定义」与「已定义」符号
  ③ 求差集：未定义 − 已定义 − 标准 C/libm/编译器内建
  ④ 差集里剩下的，就是 emcc 下会报 undefined reference 的符号

【为什么不用"直接尝试链接"】
  本机链接一定会成功（系统库兜底），所以链接通过**不能**说明问题。
  必须绕过系统库、直接看目标文件的符号表。

用法：
    python _check_web_symbols.py            # 报告
    python _check_web_symbols.py --quiet     # 只输出结论（CI 用）
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GCC = os.environ.get("CC", "gcc")

# 标准 C / libm / 编译器内建 / Emscripten 自带 —— 这些在 musl 上都有，不算缺口
STDLIB_OK = re.compile(
    r'^('
    r'__|_'                       # 编译器内建、以及 GCC/MinGW 的内部装饰名
    r'|mem|str|wcs|sn?printf|vsn?printf|fopen|fclose|fread|fwrite|fseek|ftell|fflush'
    # 宽字符版的 printf 家族：swprintf / vswprintf / fwprintf 都是 **C99 标准**，
    # musl 里有，所以属于"标准库"而不是缺口。
    # ⚠️ 第一版白名单只写了 vsn?printf，漏了 vswprintf，于是它被误报成缺口。
    #    误报比漏报更烦人 —— 会让人开始怀疑这个检查本身。
    r'|swprintf|vswprintf|wprintf|vwprintf|fwprintf|vfwprintf'
    r'|malloc|calloc|realloc|free|aligned_alloc'
    r'|sin|cos|tan|asin|acos|atan|sqrt|pow|fabs|floor|ceil|exp|log|fmod|round'
    r'|rand|srand|qsort|bsearch|abs|labs|atoi|atof|strtol|strtod'
    r'|time|clock|localtime|gmtime|mktime|difftime|strftime'
    r'|abort|exit|atexit|getenv|setenv|remove|rename|tmpfile|setvbuf|fflush'
    r'|iswalpha|iswdigit|iswspace|towlower|towupper|wctype|iswctype'
    r'|setlocale|localeconv|signal|raise'
    r')'
)

# 引擎里确实会用到、且平台层**必须**实现的（不做白名单，缺了就该报）
# 这个列表只用于在报告里给提示，不参与判定。
HINT_PREFIX = ("plat", "game", "_w", "_s", "_a", "_m", "_f", "_g", "Get", "Set",
               "Create", "Delete", "Select", "Fill", "Draw", "Ellipse", "Polygon",
               "Rectangle", "Round", "Line", "Move", "Bit", "Stretch", "Alpha",
               "Multi", "Wide", "wsprintf", "lstr", "Monitor", "Arc", "Gradient",
               "Post", "Begin", "End", "Peek", "Dispatch", "Translate", "Def",
               "Register", "Show", "Update", "Sleep", "Play", "mci", "Adjust",
               "IsWindow")


def obj_symbols(path):
    """返回 (未定义集合, 已定义集合)"""
    out = subprocess.run(["nm", path], capture_output=True, text=True).stdout
    und, dfn = set(), set()
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) < 2:
            continue
        name = parts[-1]
        if len(parts) == 2 and parts[0] == "U":
            und.add(name)
            continue
        if len(parts) >= 3:
            typ = parts[-2]
            if typ == "U":
                und.add(name)
            elif typ in ("T", "t", "D", "d", "R", "r", "B", "b", "W", "V", "i"):
                dfn.add(name)
    return und, dfn


def main():
    quiet = "--quiet" in sys.argv
    tmp = tempfile.mkdtemp(prefix="wsym_")

    objs = []
    for src in ("pvz.c", "plat_web.c"):
        p = os.path.join(HERE, src)
        if not os.path.isfile(p):
            print("  ★ 找不到 %s" % src)
            return 1
        o = os.path.join(tmp, src + ".o")
        # -O0 更快；关键是要拿到符号表。-w 压掉源码告警，
        # 因为这里只关心符号，告警由别处负责。
        r = subprocess.run(
            [GCC, "-DPLAT_PORTABLE", "-O0", "-w", "-c", p, "-o", o, "-I", HERE],
            capture_output=True, text=True, cwd=HERE)
        if r.returncode != 0:
            print("  ★ %s 编译失败：" % src)
            print("    " + (r.stderr or "").strip().replace("\n", "\n    ")[:800])
            return 1
        objs.append(o)

    und, dfn = set(), set()
    for o in objs:
        u, d = obj_symbols(o)
        und |= u
        dfn |= d

    missing = sorted(und - dfn)
    real = [m for m in missing if not STDLIB_OK.match(m)]
    std = [m for m in missing if STDLIB_OK.match(m)]

    if not quiet:
        print("== 可移植侧符号检查 ==")
        print("  目标文件：%s" % "、".join(os.path.basename(o) for o in objs))
        print("  未定义 %d 个；其中标准库 %d 个（musl 自带，无妨）"
              % (len(missing), len(std)))
        print()

    if real:
        print("  ★ 有 %d 个符号在 musl（Emscripten）上没有实现 —— emcc 会链接失败："
              % len(real))
        for m in real:
            hint = ""
            if not m.startswith(HINT_PREFIX):
                hint = "   ← 不像是平台层应该提供的，请确认来源"
            print("      %-28s%s" % (m, hint))
        print()
        print("  修法：在 plat_web.c 里补实现，并在 plat.h 的可移植分支里声明。")
        print("        （本机 MinGW 会因为 kernel32/msvcrt 兜底而**看不出**这个缺口，")
        print("          所以不能靠本机链接来验证。）")
        print("  FAILED")
        return 1

    print("  ✓ 可移植侧符号齐备（没有 musl 上缺失的实现）")
    print("  PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
