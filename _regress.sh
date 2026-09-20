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
"C:/Python314/python.exe" _mk_framedump.py >/dev/null 2>&1 && echo "  _dump.exe 已重建"

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
echo "=== ④ 可移植侧符号检查 ==="
if "$PYTHON_BIN" _check_web_symbols.py --quiet >/dev/null 2>&1; then
    echo "  PASS"
else
    echo "  ★ FAIL（跑 python _check_web_symbols.py 看是哪个符号）"
    fail=$((fail+1))
fi

echo
echo "=== ⑤ 资源核对 ==="
if "$PYTHON_BIN" _verify_assets.py --quiet >/dev/null 2>&1; then
    echo "  PASS"
else
    echo "  ★ FAIL（跑 python _verify_assets.py 看详情）"
    fail=$((fail+1))
fi

echo
echo "=== 汇总：通过 $pass / 失败 $fail / 编译问题 $cfail ==="
echo "=== 真档核对 ==="
"C:/Python314/python.exe" -c "
import struct
b=open('pvz_save.dat','rb').read()
own=struct.unpack_from('<120i',b,584)
print('  星星/金币', struct.unpack_from('<ii',b,8), ' 已拥有', sum(1 for v in own if v), '株')
" 2>/dev/null || echo "  (读不到存档)"

[ $fail -eq 0 ] && [ $cfail -eq 0 ] && exit 0 || exit 1
