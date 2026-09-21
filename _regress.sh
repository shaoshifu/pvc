#!/usr/bin/env bash
# 全套回归。**每次都必须重新编译**。
#
# 【为什么写成脚本】
#   原来回归是在命令行里手敲一个 for 循环，只跑 ./$t.exe 而不重新编译。
#   结果：某次改了 pvz.c 之后，跑的还是几天前编译出来的旧 .exe ——
#   "16/16 全过"其实是 16 个旧二进制全过，两个已经过期的测试
#   （_test_draft_odds / _test_draft_nav 的期望值没跟上卡表扩容）
#   就这样被掩盖了整整一轮。
#   教训：**回归脚本必须先编译再运行**，一个 gcc 失败就要显式报出来，
#   不能 `2>/dev/null` 把编译错误吞掉。
set -u
cd "$(dirname "$0")"

CFLAGS="-O2 -Wall -Wextra"
LIBS="-lgdi32 -luser32 -lmsimg32 -lwinmm -ld3d9 -lm"

# 需要参数的测试：名字 → 运行参数
# 资源核对脚本要用（本机是 C:/Python314，CI 上是 python3）
PYTHON_BIN="${PYTHON:-C:/Python314/python.exe}"

TESTS=(
  _test_cards50 _test_cards50_visual _test_primordial_buff _test_render_baseline
  _test_melee_only _test_relic_effect _test_relic_align _test_mech_fix
  _test_devour_plants _test_zombie_stall _test_primordial _test_hall_tier
  _test_draft_odds _test_draft_nav _test_loadout1 _test_asset_load
)

mkdir -p _celeb _ab _cardshot
pass=0; fail=0; cfail=0

echo "=== ① 编译主程序与抓帧器 ==="
if gcc $CFLAGS -mwindows -static-libgcc -o pvz_new.exe pvz.c $LIBS 2>&1 | head -20; then :; fi
if [ -f pvz_new.exe ]; then
    cp pvz_new.exe pvz.exe 2>/dev/null && echo "  pvz.exe 已更新" || echo "  pvz.exe 占用中（跳过）"
    pass=$((pass+1))
else
    echo "  ★ pvz.c 编译失败"; cfail=$((cfail+1))
fi
"$PYTHON_BIN" _mk_framedump.py >/dev/null 2>&1 && echo "  _dump.exe 已重建"

echo
echo "=== ② 编译全部测试（失败会显式报错，不再吞掉）==="
for t in "${TESTS[@]}"; do
    out=$(gcc $CFLAGS -o "$t.exe" "$t.c" $LIBS 2>&1)
    if [ -n "$out" ]; then
        echo "  ★ $t 编译告警/错误："
        echo "$out" | head -6
        cfail=$((cfail+1))
        # ★★ 编译失败必须**删掉旧的 exe**。
        #    踩过：编译不过时脚本继续跑，而 .exe 还是上次编出来的 ——
        #    于是"PASS"报的是陈旧二进制的结果，把真问题完全掩盖。
        #    （本轮 _test_primordial / _test_hall_tier 就是这样假装通过的，
        #      它们其实早就因为 RGQ_TRAITN 被删而编不过了。）
        rm -f "$t.exe"
    fi
done
[ $cfail -eq 0 ] && echo "  全部零告警"

echo
echo "=== ③ 运行 ==="
for t in "${TESTS[@]}"; do
    printf "  %-26s " "$t"
    case $t in
        _test_render_baseline)   ./$t.exe assets "$(pwd -W)/_rb_chk.bin" >/dev/null 2>&1 ;;
        _test_asset_load)        ./$t.exe assets "$(pwd -W)/_ab" >/dev/null 2>&1 ;;
        _test_primordial_buff)   ./$t.exe assets _celeb >/dev/null 2>&1 ;;
        _test_cards50_visual)    ./$t.exe assets "$(pwd -W)/_cardshot" >/dev/null 2>&1 ;;
        *)                       ./$t.exe >/dev/null 2>&1 ;;
    esac
    rc=$?
    if [ $rc -eq 0 ]; then echo "PASS"; pass=$((pass+1)); else echo "FAIL (exit=$rc)"; fail=$((fail+1)); fi
done

rm -f _rb_chk.bin

echo
echo "=== ④ wsprintfW 语义测试（平台无关）==="
# 锁的是「宽格式串里的 %s 收 wchar_t*」这条 **Windows 语义**。
# 2026-09-21 线上"素材载入 0 张、画面退化成程序化图形"就是它造成的：
# 第一版把 wsprintfW 实现成 vswprintf 委托，MinGW 走 MSVC 语义（正常），
# Emscripten 的 musl 走 POSIX 语义（%s 收 char*）→ 853 张素材静默全部加载失败。
#
# 为什么本机跑这个测试有意义：wsprintfW 现在是**自己实现的**，
# 两个平台跑同一份代码，所以本机结果对线上有预测力。
# （若有人改回委托 vswprintf，本机会因为 MSVC 语义恰好正确而漏过，
#   但 CI 的 Ubuntu 上会立刻 FAIL —— 所以这一条必须也在 CI 里跑。）
WP_EXE=_test_web_printf.exe
WP_EXTRA=""
case "$(uname -s)" in
    # MinGW 默认链接 kernel32/msvcrt，与我们的软件光栅同名符号冲突；
    # 需要允许重复定义（详情见 _build_web_local.sh 的说明）。
    MINGW*|MSYS*|CYGWIN*) WP_EXTRA="-Wl,--allow-multiple-definition" ;;
esac
WP_OUT=$(gcc -DPLAT_PORTABLE -O2 -Wall -Wextra -I. -o $WP_EXE _test_web_printf.c plat_web.c -lm $WP_EXTRA 2>&1)
if [ -n "$WP_OUT" ]; then
    echo "  ★ 编译告警/错误："
    echo "$WP_OUT" | head -8
    cfail=$((cfail+1))
    rm -f $WP_EXE          # 编译不过就删掉旧 exe，别让陈旧二进制假装通过
fi
if [ -f $WP_EXE ]; then
    if ./$WP_EXE >/dev/null 2>&1; then
        echo "  PASS"
        pass=$((pass+1))
    else
        echo "  ★ FAIL（跑 ./$WP_EXE 看是哪条用例）"
        fail=$((fail+1))
    fi
fi

echo
echo "=== ④b 文字（字形注入）端到端测试 ==="
# 判定的核心是"文字到底有没有被画到画面上"。
# 2026-09-15 线上出现"素材 853 张全载入、0 个 JS 异常，但**整屏一个字的都没有**"：
# putText() 用 DrawTextW(dc, s, **-1**, ...) 表示"按 NUL 结尾"（Win32 约定），
# 而 drawTextCore 把 n 当计数用 → `0 < -1` 恒假 → 循环体一次都不执行。
# 症状极具迷惑性：引擎照常登记字形请求（GetTextExtentPoint32W 收到的是真实长度），
# 于是"请求有、命中 0、落空 0"，看着像外壳没接上，实际是那一行把循环跳过了。
# 这个测试用"实心假字形 + 全图纯色块统计"把结论钉死，不用去浏览器里猜。
GW_EXE=_test_web_glyph.exe
GW_OUT=$(gcc -DPLAT_PORTABLE -O2 -Wall -Wextra -I. -o $GW_EXE \
         _test_web_glyph.c plat_web.c pvz.c -lm $WP_EXTRA 2>&1)
if [ -n "$GW_OUT" ]; then
    echo "  ★ 编译告警/错误："
    echo "$GW_OUT" | head -8
    cfail=$((cfail+1))
    rm -f $GW_EXE
fi
if [ -f $GW_EXE ]; then
    if ./$GW_EXE >/dev/null 2>&1; then
        echo "  PASS"
        pass=$((pass+1))
    else
        echo "  ★ FAIL（跑 ./$GW_EXE 看是哪条断言）"
        fail=$((fail+1))
    fi
fi

echo
echo "=== ⑤ 可移植侧符号检查 ==="
if "$PYTHON_BIN" _check_web_symbols.py --quiet >/dev/null 2>&1; then
    echo "  PASS"
else
    echo "  ★ FAIL（跑 python _check_web_symbols.py 看是哪个符号）"
    fail=$((fail+1))
fi

echo
echo "=== ⑥ 资源核对 ==="
if "$PYTHON_BIN" _verify_assets.py --quiet >/dev/null 2>&1; then
    echo "  PASS"
else
    echo "  ★ FAIL（跑 python _verify_assets.py 看详情）"
    fail=$((fail+1))
fi

echo
echo "=== 汇总：通过 $pass / 失败 $fail / 编译问题 $cfail ==="
echo "=== 真档核对 ==="
"$PYTHON_BIN" -c "
import struct
b=open('pvz_save.dat','rb').read()
own=struct.unpack_from('<120i',b,584)
print('  星星/金币', struct.unpack_from('<ii',b,8), ' 已拥有', sum(1 for v in own if v), '株')
" 2>/dev/null || echo "  (读不到存档)"

[ $fail -eq 0 ] && [ $cfail -eq 0 ] && exit 0 || exit 1
