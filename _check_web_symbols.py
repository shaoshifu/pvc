#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查"可移植侧"有没有漏实现符号 —— 提前发现 emcc 才会暴露的链接错误。

【为什么需要这个检查】
  2026-09-21 的 CI 里，前面几步全过，到 `编译 WASM` 失败。
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
  （若机器上真有 emcc，可以用 `--with-emcc` 做一次权威复核，见下。）

【★★ 这个脚本自己踩过两次"白名单不全"的坑，所以现在会自校验】
    · 第一次：白名单漏了 `vswprintf`（C99 标准，musl 有）→ 误报成缺口
    · 第二次：白名单用了 `iswalpha|iswdigit|iswspace` 这种**部分枚举**，
      漏了 `iswalnum` → 在 Linux CI 上误报，而本机（MinGW）看不出
      —— 因为它那边的 wctype.h 把 iswalnum 做成了宏，根本不产生符号。
      症状是"本地通过、CI 失败"，且失败原因看着像真缺符号。

   教训：**一个会误报的检查，最后一定会被人忽略 —— 那比没有检查更糟。**
   所以现在做三件事：
     ① 白名单改成**精确名单**（不再用 `mem|str|wcs` 这种前缀匹配 ——
        前缀匹配看着省事，但会把 `strangeThing` 这类平台函数一起放过去，
        那才是真的危险：**漏报**）
     ② 脚本内置一份 ISO C 标准函数全集，启动时断言白名单能覆盖它
        （`SELF_TEST_NAMES`）。名单再不全会**当场**被抓出来，
        而不是在生产环境里变成一个假的失败。
     ③ 报"缺符号"时同时提示"也可能是白名单漏了"，并给出验证命令。

用法：
    python _check_web_symbols.py            # 报告
    python _check_web_symbols.py --quiet     # 只输出结论（CI / 回归脚本用）
    python _check_web_symbols.py --with-emcc # 有 emcc 时做一次权威复核（较慢）
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GCC = os.environ.get("CC", "gcc")

# ─────────────────────────── 标准库白名单（精确名单）───────────────────────────
# 只用**精确名字**，不用前缀。理由见文件头"白名单不全"那段：
# 前缀匹配会顺带放过同前缀的平台函数，那是"漏报"，比误报危险得多。
# math.h 的函数在 C99 里都有 f / l 后缀版本（musl 全都有）。
# ⚠️ 手写枚举必然漏 —— 实测漏了 atan2f/ceilf/cosf/sinf/sqrtf 五个。
#    所以**按标准规则程序化生成**：后缀约定是 C 标准的一部分，
#    这不是"宽松的前缀匹配"，而是照着标准补全。
# ⚠️ 这份列表要照着 **C99/C11 的 math.h 全集**写。
#    实测漏过 ceil / floor（重写时手抖漏了），脚本立刻报出来 —— 这正是精确名单的价值。
MATH_BASE = """
sin cos tan asin acos atan atan2
sinh cosh tanh asinh acosh atanh
exp exp2 expm1 log log10 log2 log1p
pow sqrt cbrt hypot fabs
ceil floor round trunc nearbyint rint
lrint llrint lround llround
fmod remainder remquo
frexp ldexp modf scalbn scalbln ilogb logb
copysign nan nextafter nexttoward
fdim fmax fmin fma
erf erfc tgamma lgamma
""".split()
MATH_ALL = set()
for _b in MATH_BASE:
    MATH_ALL |= {_b, _b + "f", _b + "l"}

STDLIB_NAMES = set(MATH_ALL)
STDLIB_NAMES |= set("""
memcpy memmove memset memcmp memchr
strcpy strncpy strcat strncat strcmp strncmp strcoll strxfrm
strchr strrchr strspn strcspn strpbrk strstr strtok strerror strlen
wcscpy wcsncpy wcscat wcsncat wcscmp wcsncmp wcscoll wcsxfrm
wcschr wcsrchr wcsspn wcscspn wcspbrk wcsstr wcstok wcslen
wmemcpy wmemmove wmemset wmemcmp wmemchr
wcstol wcstoul wcstoll wcstoull wcstof wcstod wcstold
mbrtowc wcrtomb mbsrtowcs wcsrtombs mbrlen mbsinit btowc wctob
mblen mbtowc wctomb mbstowcs wcstombs
fopen fclose fread fwrite fseek ftell fflush fgetc fgets fputc fputs
getc putc getchar putchar gets puts
printf fprintf sprintf snprintf vprintf vfprintf vsprintf vsnprintf
scanf fscanf sscanf puts perror
remove rename tmpfile tmpnam setbuf setvbuf
fgetwc fgetws fputwc fputws getwc getwchar putwc putwchar ungetwc
fwprintf wprintf swprintf vfwprintf vwprintf vswprintf
fwscanf wscanf swscanf vfwscanf vwscanf vswscanf
malloc calloc realloc free aligned_alloc
abort atexit exit getenv system
atoi atol atoll atof strtol strtoll strtoul strtoull strtof strtod strtold
rand srand qsort bsearch
abs labs llabs div ldiv lldiv
sin cos tan asin acos atan atan2 sinh cosh tanh
exp log log10 pow sqrt ceil floor fabs fmod frexp ldexp modf
round trunc nearbyint rint copysign fmin fmax fma hypot cbrt exp2 log2
expm1 log1p exp10
clock time difftime mktime strftime gmtime localtime asctime ctime
signal raise setjmp longjmp
setlocale localeconv
isalnum isalpha isblank iscntrl isdigit isgraph islower isprint
ispunct isspace isupper isxdigit tolower toupper
iswalnum iswalpha iswblank iswcntrl iswdigit iswgraph iswlower iswprint
iswpunct iswspace iswupper iswxdigit towlower towupper
iswctype wctype towctrans wctrans
""".split())

# ── MinGW 头文件"重命名"的产物（不是缺口，也不是标准库名字）───────────────
# ★★ 这是本检查的一个**系统性局限**，必须显式标注，否则会一直误报：
#    同一个源文件，在两个平台下**产生的符号名不同**。
#      源码写 `time(NULL)`  → MinGW 的 time.h 把它变成 `_time64`
#                            → musl 那边仍然是 `time`
#      源码写 `assert(0)`   → MinGW 变成 `_assert`
#                            → musl 变成 `__assert_fail`
#    两者各自都能链接成功。而本检查是"拿 MinGW 产出的符号名去问 musl 有没有" ——
#    对这类被头文件改过名的符号，这个问题本身就问错了。
#
#    所以它们既不能进 STDLIB_NAMES（那会掩盖真的平台符号），
#    也不该判为失败。单独一档，只提示。
#
#    ⚠️ 真正的权威判断是 `--with-emcc`（真让 emcc 链一次）。
#      有 emcc 的机器上应当优先看它。
MINGW_HEADER_REMAP = set("""
_time64 _localtime64 _gmtime64 _ctime64 _ftime64 _utime64
_assert _wassert
_findfirst64 _findnext64 _findfirst64i32 _findnext64i32
_stati64 _wstati64 _fstati64 _wstat64
_spawnv _spawnve _wspawnv _wspawnve
""".split())

# 自校验用的名字集合：**必须是上面 STDLIB_NAMES 的子集**，
# 而且必须都是 ISO C / libm 真正提供、musl 里也确实有的。
# 它的存在意义：把"名单不全"从"生产环境里的假失败"变成"启动时的真失败"。
SELF_TEST_NAMES = set("""
memcpy memset memcmp memchr strcpy strlen strcmp strstr strchr strrchr
wcscpy wcslen wcscmp wcschr wmemcpy
fopen fclose fread fwrite fseek ftell fflush
printf sprintf snprintf vprintf vsnprintf fprintf
swprintf vswprintf fwprintf wprintf
malloc calloc realloc free
atoi atof strtol strtod rand srand qsort bsearch abs labs
sin cos tan sqrt pow fabs floor ceil exp log fmod round
time clock mktime strftime localtime difftime
abort exit getenv setlocale localeconv signal raise
isalnum isalpha isdigit isspace isupper islower isxdigit tolower toupper
iswalnum iswalpha iswdigit iswspace iswupper iswlower iswxdigit towlower towupper
iswctype wctype
ceil floor sqrt sin cos atan2
ceilf floorf sqrtf sinf cosf atan2f
ceill floorl sqrtl sinl cosl atan2l
""".split())

# 编译器 / 平台内部装饰名。这些不是"标准库函数"，而是 ABI 层面的内部符号，
# 目标平台上由编译器运行时提供（emscripten 也有）。用**窄**前缀，别写成 `_`。
INTERNAL_PREFIXES = (
    "__",           # GCC/Clang 内建：__udivdi3、__stack_chk_fail、__assert_fail…
    "_Unwind",      # 异常展开
    "_GLOBAL__",    # 静态初始化
    "emscripten_",  # emcc 自带
    "em_",          # emcc 自带
)

# 另一类"平台特有产物"：只在某个平台的目标文件里出现的链接器/ABI 符号。
# 它们既不是标准库、也不是缺口 —— 换一个平台编译就根本不存在。
#   _GLOBAL_OFFSET_TABLE_  Linux/PIC 的 GOT 基准符号，**Windows 上不存在**
#                          （只在 Linux 的 gcc 输出里出现，换到 MinGW 就没了）
# 这类名字要**精确列出**（不要用前缀，避免放过真的平台函数）。
PLATFORM_LINKER_ARTIFACTS = {
    "_GLOBAL_OFFSET_TABLE_",
    "_GLOBAL_OFFSET_TABLE",     # 某些 ABI 变体
}


def is_stdlib(name):
    """这个符号是否由标准 C / libm / 编译器运行时提供（musl 上也有）？"""
    if name in STDLIB_NAMES:
        return True
    if name in PLATFORM_LINKER_ARTIFACTS:
        return True
    for p in INTERNAL_PREFIXES:
        if name.startswith(p):
            return True
    return False


def self_test():
    """断言白名单能覆盖一份 ISO C 标准函数全集。

    为什么要有这一步：这个白名单**已经漏过两次**（vswprintf、iswalnum），
    每次的表现都是"在某个平台上误报成缺符号"，而要花很久才看出
    "其实是我的名单不全"。加了自校验之后，同样的疏漏会在**启动时**
    立刻报出来，且信息直指修法。
    """
    gap = sorted(n for n in SELF_TEST_NAMES if not is_stdlib(n))
    return gap


# 引擎里确实会用到、且平台层**必须**实现的（缺了就该报）。
# 这个列表只用于在报告里给提示，不参与判定。
HINT_PREFIX = ("plat", "game", "_w", "_s", "_a", "_m", "_f", "_g", "Get", "Set",
               "Create", "Delete", "Select", "Fill", "Draw", "Ellipse", "Polygon",
               "Rectangle", "Round", "Line", "Move", "Bit", "Stretch", "Alpha",
               "Multi", "Wide", "wsprintf", "lstr", "Monitor", "Arc", "Gradient",
               "Post", "Begin", "End", "Peek", "Dispatch", "Translate", "Def",
               "Register", "Show", "Update", "Sleep", "Play", "mci", "Adjust",
               "IsWindow")


def find_nm():
    """找一个可用的符号表工具。

    ★ 这里必须容错，而且**找不到时要报 SKIP 而不是 FAIL**。
      第一版直接 `subprocess.run(["nm", ...])`，在缺 nm 的环境里抛
      FileNotFoundError → 脚本非零退出 → CI 步骤失败。
      症状极具误导性：它看起来像"代码里有缺失符号"，
      实际只是工具没装。**一个会因工具缺失而误报的检查，
      最后一定会被人忽略 —— 那比没有检查更糟。**
    """
    for tool, args in (("nm", []), ("llvm-nm", []), ("objdump", ["-t"])):
        if shutil.which(tool) is None:
            continue
        return tool, args
    return None, None


def obj_symbols(path, tool, args):
    """返回 (未定义集合, 已定义集合)；工具不可用时返回 (None, None)"""
    try:
        out = subprocess.run([tool] + args + [path],
                             capture_output=True, text=True).stdout
    except (FileNotFoundError, OSError):
        return None, None
    if not out:
        return None, None

    und, dfn = set(), set()
    is_objdump = bool(args)          # objdump -t 的输出格式不同
    for ln in out.splitlines():
        parts = ln.split()
        if not parts:
            continue
        if is_objdump:
            # objdump -t 形如：`0000000000000000         *UND*	0000000000000000 foo`
            if "*UND*" in ln:
                und.add(parts[-1])
            elif len(parts) >= 6 and parts[-1] != "*UND*":
                dfn.add(parts[-1])
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


def compile_objs(tmp):
    """把可移植侧编成目标文件。返回 (objs, 错误信息 or None)"""
    objs = []
    for src in ("pvz.c", "plat_web.c"):
        p = os.path.join(HERE, src)
        if not os.path.isfile(p):
            return None, "找不到 %s" % src
        o = os.path.join(tmp, src + ".o")
        # -O0 更快；关键是要拿到符号表。-w 压掉源码告警，
        # 因为这里只关心符号，告警由别处负责。
        r = subprocess.run(
            [GCC, "-DPLAT_PORTABLE", "-O0", "-w", "-c", p, "-o", o, "-I", HERE],
            capture_output=True, text=True, cwd=HERE)
        if r.returncode != 0:
            return None, ("%s 编译失败：\n    " % src +
                          (r.stderr or "").strip().replace("\n", "\n    ")[:800])
        objs.append(o)
    return objs, None


def verify_with_emcc():
    """★ 权威复核：真让 emcc 链一次。

    白名单永远是"启发式"，而**链接器是事实**。
    机器上有 emcc 时（比如编译服务器），这个是唯一不会有争议的判据：
    它直接回答"能不能链上"，不依赖任何名单。
    返回 (verdict, 输出)：verdict ∈ {"pass","fail","skip"}
    """
    if shutil.which("emcc") is None:
        return "skip", "（PATH 里没有 emcc）"
    out = os.path.join(tempfile.mkdtemp(prefix="wemcc_"), "probe.js")
    cmd = [GCC if False else "emcc", "-DPLAT_PORTABLE", "-O0", "-w", "-I.", "-sWASM=1",
           "-sALLOW_MEMORY_GROWTH=1", "-sFORCE_FILESYSTEM=1", "--no-entry",
           "-sEXPORTED_FUNCTIONS=[]", "-o", out, "pvz.c", "plat_web.c"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=HERE)
    txt = (r.stdout or "") + (r.stderr or "")
    if r.returncode == 0:
        return "pass", "emcc 链接通过（这是权威判据）"
    return "fail", txt.strip()[-1200:]


def main():
    quiet = "--quiet" in sys.argv
    with_emcc = "--with-emcc" in sys.argv

    # ① 先自校验白名单 —— 名单不全就不该继续，否则会报出一堆假失败
    gap = self_test()
    if gap:
        print("  ★ 符号白名单不完整（自校验失败）：%s" % "、".join(gap))
        print("    这些是 ISO C 标准函数，musl 上都有，却被判成'缺失'。")
        print("    修法：把它们加进本文件的 STDLIB_NAMES。")
        print("    ⚠️ 这正是 vswprintf / iswalnum 两次误报的同一类原因 ——")
        print("       名单不全时，这个检查会在某个平台上产生**看似真实**的失败。")
        print("  FAILED")
        return 1

    tmp = tempfile.mkdtemp(prefix="wsym_")
    tool, targs = find_nm()
    if tool is None:
        print("  ⚠ 找不到 nm / objdump，**跳过**符号检查（不判为失败）")
        print("    在 CI 上请确保装了 binutils：apt-get install -y binutils")
        print("  SKIP")
        return 0

    objs, err = compile_objs(tmp)
    if err:
        print("  ★ %s" % err)
        return 1

    und, dfn = set(), set()
    for o in objs:
        u, d = obj_symbols(o, tool, targs)
        if u is None:
            print("  ⚠ %s 符号表读取失败，**跳过**检查（不判为失败）" % os.path.basename(o))
            print("  SKIP")
            return 0
        und |= u
        dfn |= d

    missing = sorted(und - dfn)
    std = [m for m in missing if is_stdlib(m)]
    remap = [m for m in missing if not is_stdlib(m) and m in MINGW_HEADER_REMAP]
    real = [m for m in missing
            if not is_stdlib(m) and m not in MINGW_HEADER_REMAP]

    if not quiet:
        print("== 可移植侧符号检查 ==")
        print("  工具：%s" % tool)
        print("  目标文件：%s" % "、".join(os.path.basename(o) for o in objs))
        print("  未定义 %d 个；其中标准库 %d 个、MinGW 头文件重命名 %d 个"
              % (len(missing), len(std), len(remap)))
        if remap:
            print("      重命名的（MinGW 与 musl 各自链接成功，非缺口）：%s"
                  % "、".join(remap))
        print()

    verdict, emcc_txt = ("skip", "")
    if with_emcc or real:
        verdict, emcc_txt = verify_with_emcc()
        if not quiet:
            print("  权威复核（emcc 实链）：%s" % verdict)
            if verdict != "pass" and emcc_txt and not quiet:
                print("    " + emcc_txt.replace("\n", "\n    ")[:900])
            print()

    if real and verdict == "pass":
        # ★★ 权威优先：emcc 真链过了，说明这些不是缺口。
        #    最可能的原因是"白名单漏项"或"MinGW 头文件改名"这类
        #    **检查自身的局限**，而不是代码问题。
        #    早先的版本在这里仍然 return 1 —— 那就变成"权威说行、启发式说不行，
        #    结果按启发式判"，与"emcc 才是权威"的设计直接矛盾。
        if not quiet:
            print("  ⚠ 启发式名单认为下面这些缺实现，但 **emcc 实测链接通过**：")
            for m in real:
                print("      %s" % m)
            print("    → 结论：它们不是缺口。最可能是白名单漏项（请补进 STDLIB_NAMES）")
            print("      或 MinGW 头文件改名（请补进 MINGW_HEADER_REMAP）。")
            print()

    if real and verdict != "pass":
        print("  ★ 有 %d 个符号在 musl（Emscripten）上没有实现 —— emcc 会链接失败："
              % len(real))
        for m in real:
            hint = ""
            if not m.startswith(HINT_PREFIX):
                hint = "   ← 不像是平台层应该提供的，请确认来源"
            print("      %-28s%s" % (m, hint))
        print()
        print("  两种可能，先分辨是哪种（**别急着去补实现**）：")
        print("    ① 真的缺：在 plat_web.c 里补实现，并在 plat.h 的可移植分支里声明。")
        print("    ② 白名单漏了：它其实是标准库函数。验证办法 ——")
        print("       `python _check_web_symbols.py --with-emcc`")
        print("       若 emcc 能链上，就是白名单问题，把它加进 STDLIB_NAMES。")
        print("       （白名单已漏过两次：vswprintf、iswalnum）")
        print()
        print("  本机 MinGW 会因为 kernel32/msvcrt 兜底而**看不出**这个缺口，")
        print("  所以不能靠本机链接来验证。")

        if verdict == "skip":
            print()
            print("  （机器上没有 emcc，无法做权威复核 —— 只能按上面的名单判断。")
            print("    在编译服务器上跑 `--with-emcc` 可以得到无争议的结论。）")
        print("  FAILED")
        return 1

    if verdict == "fail":
        print("  ★ emcc 实测链接**失败**（这是权威判据，不再依赖名单）：")
        print("    " + emcc_txt.replace("\n", "\n    ")[:900])
        print("  FAILED")
        return 1

    print("  ✓ 可移植侧符号齐备（没有 musl 上缺失的实现）")
    if verdict == "pass":
        print("  ✓ emcc 权威复核通过")
    print("  PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
