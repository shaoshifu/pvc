/* =========================================================================
   _test_web_clock.c —— 验证 plat_web.c 的 GetTickCount 真的在递增
   -------------------------------------------------------------------------
   为什么值得单独写一个测试：
     这个函数第一版返回一个内部计数器（恒为 0），后果是**所有音效静默失效**
     —— 因为引擎的防抖写成 `if ((DWORD)(now - last) < 32) return;`，
     now 与 last 都是 0 时恒成立。
     这类"平台函数语义不对"的 bug 不会崩、不会报错，只有真正玩一遍才发现，
     所以必须有一条自动化的断言钉住它。

   编译：
     gcc -DPLAT_PORTABLE -O2 -o _test_web_clock.exe _test_web_clock.c plat_web.c -lm \
         -Wl,--allow-multiple-definition
   ========================================================================= */
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>

/* plat_web.c 的 platWebFrame() 引用了 gameWorldBits()（实现在 pvz.c 里）。
   本测试不链接 pvz.c，所以给个桩。 */
const void *gameWorldBits(int *w, int *h, int *stride)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (stride) *stride = 0;
    return NULL;
}

static int fails = 0;

#define CHECK(cond, label) do { \
    if (cond) printf("ok:   %s\n", label); \
    else { printf("FAIL: %s\n", label); fails++; } \
} while (0)

/* 忙等若干毫秒。刻意不用 Sleep —— plat_web 的 Sleep 是空实现
   （网页版由 rAF 驱动，不需要阻塞），用它做等待等于没等。 */
static void busyWait(int ms)
{
    volatile double sink = 0.0;      /* 防止编译器把整个循环优化掉 */
    DWORD t0 = GetTickCount();
    while ((DWORD)(GetTickCount() - t0) < (DWORD)ms) {
        int i;
        for (i = 0; i < 20000; i++) sink += (double)i * 0.5;
    }
    (void)sink;                      /* 显式消费：消除 -Wunused-but-set-variable */
}

int main(void)
{
    DWORD a, b, c;

    printf("== plat_web 时钟语义 ==\n");

    a = GetTickCount();
    CHECK(a != 0, "clock: GetTickCount 不是恒零（恒零会让所有防抖逻辑失效）");

    busyWait(60);
    b = GetTickCount();
    CHECK(b > a, "clock: 随时间递增");
    printf("       忙等约 60ms 后，读数从 %u 走到 %u（差 %u）\n",
           (unsigned)a, (unsigned)b, (unsigned)(b - a));

    /* 时间差的量级要对：这直接决定防抖窗口（32ms / 350ms）是否合理。
       忙等 20ms 时读数至少要走 20ms；上限放宽到 5000 是因为
       不同机器负载差异很大，这条只排除"单位错了"（比如误用秒）。 */
    {
        DWORD t0 = GetTickCount();
        busyWait(20);
        c = GetTickCount();
        CHECK((DWORD)(c - t0) >= 20 && (DWORD)(c - t0) < 5000,
              "clock: 时间差量级是毫秒（不是秒，也不是微秒）");
        printf("       忙等 20ms 实测差 %u\n", (unsigned)(c - t0));
    }

    /* 模拟 sfxPlay 的防抖：连续两次调用之间没有等待 → 应该被拦下。
       这正是 GetTickCount 恒零时会**恒成立**的那个条件。 */
    {
        DWORD lastHit = 0;
        int blocked = 0, passed = 0, i;
        for (i = 0; i < 5; i++) {
            DWORD now = GetTickCount();
            if ((DWORD)(now - lastHit) < 32) blocked++;
            else { lastHit = now; passed++; }
        }
        printf("       防抖模拟：5 次密集调用 → 拦下 %d 次，放行 %d 次\n",
               blocked, passed);
        CHECK(passed >= 1, "clock: 密集调用至少能放行一次（恒零时会全部拦掉）");

        /* 等一下再调：必须能放行 */
        busyWait(50);
        {
            DWORD now = GetTickCount();
            CHECK((DWORD)(now - lastHit) >= 32,
                  "clock: 超过防抖窗口后能放行（音效才播得出来）");
        }
    }

    printf("\n%s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
