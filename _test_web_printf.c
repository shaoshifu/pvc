/* ============================================================================
   wsprintfW 单元测试 —— 锁住 **Windows 语义**

   【为什么必须有这个测试】
   2026-09-21 网页版出现"素材载入 0 张、画面退化成程序化图形"，
   根因就是 wsprintfW 第一版实现成了 `vswprintf(out, 512, fmt, ap)`：
     · 宽格式串里的 `%s`，MSVC 语义收 `wchar_t*`，POSIX 语义收 `char*`
     · 本机 MinGW 走 MSVC 语义 → 一切正常 → 本地验证全绿
     · Emscripten 的 musl 走 POSIX 语义 → 把 wchar_t* 当 char* 读 →
       拼出垃圾路径 → 853 张素材全部加载失败，且**不报任何错**
   这是典型的"本地过、目标平台失败"：链接检查（_check_web_symbols.py）抓不到
   （符号存在），编译告警也抓不到（语法完全合法）。

   【为什么这个测试在本机跑是有意义的】
   只因为 wsprintfW 现在是**自己实现**的：两个平台跑的是同一份代码。
   如果哪天有人图省事改回 `vswprintf(...)` 委托：
     · 在 Linux/musl 上 → 本测试立刻 FAIL（第 ① 组用例会输出垃圾）
     · 在 Windows 上 → 仍然 PASS（MSVC 语义恰好对）
   所以**必须在 CI（Ubuntu）上跑**才有回归防护力；本机跑只是快速反馈。
   末尾有一条平台语义自检，把这件事显式记录在输出里。

   【输出为什么全用 ASCII】
   这个测试可能在 Windows 控制台（GBK 代码页）、CI 日志、重定向文件里跑。
   中文一旦遇到编码不匹配就变成乱码，把"哪条用例失败"这个关键信息毁掉。
   需要比对中文宽字符串的用例，失败时改印 **码点**（U+XXXX），任何环境都可读。

   【编译方式】
     gcc -DPLAT_PORTABLE -O2 -I. -o _test_web_printf.exe \
         _test_web_printf.c plat_web.c -lm [-Wl,--allow-multiple-definition]
   刻意**不碰 plat_web.c 的结构**：测试作为独立可执行文件与 plat_web.o 链接。
   CI 与 _regress.sh 都直接跑它。

   引擎实际用到的格式符只有 6 种（实测统计）：
     %d(62处) %s(38处) %02d(4处) %.2f(4处) %.0f(3处) %%
   全部覆盖在下面。
   ============================================================================ */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "plat.h"

static int nPass = 0, nFail = 0;

/* 是否纯 ASCII（可安全直接打印） */
static int isAscii(const wchar_t *s)
{
    for (; s && *s; s++)
        if (*s < 0x20 || *s > 0x7e) return 0;
    return 1;
}

/* 把宽字符串按码点打印：不含编码假设，任何控制台都能读 */
static void dumpW(const wchar_t *s)
{
    printf("U+[");
    for (; s && *s; s++) printf("%04X ", (unsigned)*s);
    printf("]");
}

static void showW(const wchar_t *s)
{
    if (s && isAscii(s)) printf("\"%ls\"", s);
    else                 dumpW(s);
}

static void ck(const char *what, const wchar_t *got, const wchar_t *want)
{
    if (wcscmp(got, want) == 0) {
        nPass++;
        printf("  OK   %-50s -> ", what);
        showW(got);
        printf("\n");
    } else {
        nFail++;
        printf("  FAIL %s\n", what);
        printf("         got  : "); showW(got);  printf("\n");
        printf("         want : "); showW(want); printf("\n");
    }
}

static void ckInt(const char *what, int got, int want)
{
    if (got == want) {
        nPass++;
        printf("  OK   %-50s -> %d\n", what, got);
    } else {
        nFail++;
        printf("  FAIL %-50s  got %d / want %d\n", what, got, want);
    }
}

/* plat_web.c 里 platWebFrame() 会引用引擎侧的 gameWorldBits()（定义在 pvz.c）。
   本测试只链接 plat_web.c，所以要给一个桩。
   返回 NULL 是安全的：wsprintfW 的测试根本不走那条路径。 */
const void *gameWorldBits(int *w, int *h, int *stride)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (stride) *stride = 0;
    return NULL;
}

int main(void)
{
    wchar_t b[512];
    int n;
    int mn = -2147483647 - 1;        /* INT_MIN，不能写字面量 -2147483648 */

    /* ---- ① 核心：%s 收 wchar_t*（这正是线上出问题的那一类）--------------
       引擎里有 38 处，最要命的是拼资源路径：
           wsprintfW(path, L"%s%s.png", gAssetDir, name);          (pvz.c:4725)
           wsprintfW(path, L"%ssfx\\%s.wav", gAssetDir, name);      (pvz.c:4294)
       如果 %s 按 POSIX 语义解析，这两行会产出垃圾路径且**不报错**。 */
    printf("\n(1) %%s must be Windows semantics (takes wchar_t*, not char*)\n");
    wsprintfW(b, L"%s%s.png", L"assets/", L"sunflower");
    ck("L\"%s%s.png\" + (assets/, sunflower)", b, L"assets/sunflower.png");

    wsprintfW(b, L"%s%s", L"assets/", L"rgplant_00");
    ck("L\"%s%s\" two-part concat", b, L"assets/rgplant_00");

    /* 反斜杠是**刻意保留**的：pvz.c 用 Windows 风格拼路径，
       由 _wfopen 负责转成正斜杠给 MEMFS。这里若被改动就是回归。 */
    wsprintfW(b, L"%ssfx\\%s.wav", L"assets/", L"hit");
    ck("L\"%ssfx\\\\%s.wav\" (backslash preserved)", b, L"assets/sfx\\hit.wav");

    /* 中文宽字符串：走多字节宽字符路径。
       标签用 ASCII，失败时印码点 —— 免得乱码盖掉真正的信息。 */
    printf("\n(2) wide (non-ASCII) strings\n");
    wsprintfW(b, L"%s", L"\u4e2d\u6587");           /* 中文 */
    ck("L\"%s\" + U+4E2D U+6587", b, L"\u4e2d\u6587");

    wsprintfW(b, L"%s%s", L"\u9633\u5149 ", L"\u6a21\u5f0f");   /* 阳光 / 模式 */
    ck("L\"%s%s\" CJK concat", b, L"\u9633\u5149 \u6a21\u5f0f");

    wsprintfW(b, L"%s", L"");
    ck("empty string arg", b, L"");

    wsprintfW(b, L"a%sb", NULL);
    ck("NULL arg (Windows behaviour = (null))", b, L"a(null)b");

    /* 参数里的 % 不能被当成格式符**二次解析** —— printf 家族的经典陷阱 */
    wsprintfW(b, L"%s", L"100%");
    ck("percent inside arg must not be re-parsed", b, L"100%");

    /* ---- ③ %d / 整数 ------------------------------------------------ */
    printf("\n(3) %%d and integers\n");
    wsprintfW(b, L"%d", 250);
    ck("L\"%d\"", b, L"250");

    wsprintfW(b, L"%d", -7);
    ck("%d negative", b, L"-7");

    wsprintfW(b, L"%d", 0);
    ck("%d zero", b, L"0");

    wsprintfW(b, L"%d", mn);
    ck("%d INT_MIN (tests the -(v+1)+1 negation trick)", b, L"-2147483648");

    wsprintfW(b, L"%s%d", L"gold ", 12345);
    ck("%s mixed with %d", b, L"gold 12345");

    wsprintfW(b, L"%u", 4294967295u);
    ck("%u UINT_MAX (must be unsigned, not -1)", b, L"4294967295");

    wsprintfW(b, L"%u", 0u);
    ck("%u zero", b, L"0");

    wsprintfW(b, L"%lu", 3000000000u);
    ck("%lu beyond INT_MAX", b, L"3000000000");

    wsprintfW(b, L"%x", 48879u);
    ck("%x lowercase hex", b, L"beef");

    wsprintfW(b, L"%X", 48879u);
    ck("%X uppercase hex", b, L"BEEF");

    wsprintfW(b, L"%08x", 255u);
    ck("%08x zero-padded hex", b, L"000000ff");

    /* ---- ④ %02d ----------------------------------------------------- */
    printf("\n(4) %%02d (level / index padding)\n");
    wsprintfW(b, L"%02d", 7);
    ck("%02d pad", b, L"07");

    wsprintfW(b, L"%02d", 12);
    ck("%02d already wide", b, L"12");

    wsprintfW(b, L"%02d", -5);
    ck("%02d negative (no padding needed)", b, L"-5");

    /* ★ 这两条抓出过一个真 bug：第一版把符号当普通字符一起补位，
       于是 %03d 传 -5 得到 "0-5"。正确是 "-05"（零补在符号之后）。 */
    wsprintfW(b, L"%03d", -5);
    ck("%03d negative (must be -05, not 0-5)", b, L"-05");

    wsprintfW(b, L"%05d", -42);
    ck("%05d negative (must be -0042)", b, L"-0042");

    wsprintfW(b, L"%3d", 7);
    ck("%3d space padding (not zero)", b, L"  7");

    /* ---- ⑤ 浮点 ----------------------------------------------------- */
    printf("\n(5) floats (delegated to snprintf(\"%%.*f\") then widened)\n");
    /* 取值刻意选**二进制可精确表示**的（2.5 / 4.25 / 7.0），
       避免 glibc 与 msvcrt 在"半个 ulp"上的舍入差异造成假失败。 */
    wsprintfW(b, L"%.2f", 2.5);
    ck("%.2f 2.5", b, L"2.50");

    wsprintfW(b, L"%.2f", 4.25);
    ck("%.2f 4.25", b, L"4.25");

    wsprintfW(b, L"%.0f", 100.0);
    ck("%.0f 100.0", b, L"100");

    wsprintfW(b, L"HP %.0f", 7.0);
    ck("L\"HP %%.0f\" 7.0", b, L"HP 7");

    wsprintfW(b, L"sun %d  cd %.0f  hp %.0f", 250, 7.0, 100.0);
    ck("three specifiers mixed (pvz.c:5087 real case)", b,
       L"sun 250  cd 7  hp 100");

    wsprintfW(b, L"%f", 1.5);
    ck("%f default precision 6", b, L"1.500000");

    /* ---- ⑥ %% 与边界 ------------------------------------------------ */
    printf("\n(6) percent signs and edge cases\n");
    wsprintfW(b, L"%d%%", 50);
    ck("L\"%d%%\" literal percent", b, L"50%");

    wsprintfW(b, L"100%%");
    ck("100%%", b, L"100%");

    wsprintfW(b, L"abc%");
    ck("trailing lone %% (must not read past end)", b, L"abc%");

    wsprintfW(b, L"%z");
    ck("unknown specifier %%z (emitted verbatim)", b, L"%z");

    wsprintfW(b, L"");
    ck("empty format", b, L"");

    n = wsprintfW(b, L"abc");
    ckInt("return value = chars written", n, 3);

    n = wsprintfW(b, L"%s", L"\u4e2d\u6587");
    ckInt("%s CJK return value (2 wide chars)", n, 2);

    n = wsprintfW(b, L"%d", 12345);
    ckInt("%d return value", n, 5);

    /* ---- ⑦ 环境自检：证明这个测试在 Linux 上真的有效 ------------------
       在 POSIX（musl/glibc）上，宽格式串里的 %s 收的是 char*。
       所以如果有人把实现改回 `vswprintf` 委托，上面第 ① 组用例
       在 Linux 上会立刻失败 —— 这就是回归防护力所在。
       在 Windows/MSVC 语义下这句会输出 "x" 之后跟垃圾，属正常，
       故只在 POSIX 上断言。 */
#if defined(__linux__) || defined(__EMSCRIPTEN__) || defined(__APPLE__)
    printf("\n(7) platform check (POSIX only)\n");
    {
        wchar_t posix[64];
        swprintf(posix, 64, L"[%s]", "x");
        ck("POSIX: swprintf(L\"[%s]\", \"x\")", posix, L"[x]");
        printf("      => %s takes char* on this platform, so if wsprintfW ever\n", "%s");
        printf("         delegates to vswprintf, group (1) fails here. Regression is caught.\n");
    }
#else
    printf("\n(7) platform check: this is Windows/MSVC semantics (%s takes wchar_t*).\n", "%s");
    printf("    => a local run is only quick feedback. The real regression guard is\n");
    printf("       CI on Ubuntu, where delegating to vswprintf fails immediately.\n");
#endif

    printf("\n=== pass %d / fail %d ===\n", nPass, nFail);
    if (nFail) {
        printf("wsprintfW semantics drifted. Remember: %%s must take wchar_t* (Windows\n");
        printf("semantics). The Windows/POSIX difference raises no error at all -- it just\n");
        printf("makes all 853 sprites fail to load silently.\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
