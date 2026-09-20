# -*- coding: utf-8 -*-
"""P3-C 文案同步：修正遗物说明里与代码不符的可见文字，并保持列对齐 / CRLF 不变。"""
import sys

P = 'pvz.c'
data = open(P, 'rb').read().decode('utf-8')
assert data.count('\r\n') == data.count('\n'), '行尾不纯 CRLF，先停下检查'
lines = data.split('\r\n')


def dw(s):
    """显示宽度：CJK/全角记 2，其余记 1。"""
    return sum(2 if ord(c) > 0x2000 else 1 for c in s)


def rewrap(line, old_desc, new_desc):
    """把行里的 old_desc 换成 new_desc，并按显示宽度差补偿尾随空格保对齐。"""
    if old_desc not in line:
        return None
    pad_diff = dw(new_desc) - dw(old_desc)
    # 该行形如: { L"名",   L"描",   <pad>, 稀有, 类别 },
    head, rest = line.split(old_desc, 1)
    # rest = '",   <spaces>2, 4 },'
    i = 0
    while i < len(rest) and rest[i] != ' ':
        i += 1                      # 跳过收尾引号与逗号
    j = i
    while j < len(rest) and rest[j] == ' ':
        j += 1
    pad = j - i
    newpad = max(1, pad - pad_diff)
    return head + new_desc + rest[:i] + ' ' * newpad + rest[j:]


EDITS = [
    # (锚点, 旧描述, 新描述, 说明)
    ('L"扩容卡组"',
     'L"出战卡槽 +2（可带 10 张）"',
     'L"出战卡槽 +2（候选 7 / 实战 6）"',
     '旧文案 10 张 —— 实际上限 6'),
    ('L"防滑"',
     'L"所有植物最大生命 +30%"',
     'L"坚果 / 地刺最大生命 +80%"',
     '旧文案范围+数值都错（gBulwarkHpMul 只作用于坚果/地刺，且是 1.80 倍）'),
    ('L"绿洲"',
     'L"阳光产出翻倍"',
     'L"天空阳光掉落间隔 -25%"',
     '旧文案说翻倍，实际 gSkySunMul*=0.75'),
    ('L"夜灯"',
     'L"光照术：所有关卡阳光掉落加速 25%"',
     'L"天空阳光掉落间隔 -25%"',
     '统一到「晴空」同款措辞，暴露与「绿洲」效果重复'),
]

for anchor, old, new, why in EDITS:
    hit = [k for k, ln in enumerate(lines) if anchor in ln and old in ln]
    if len(hit) != 1:
        print('!! %s 命中 %d 处，跳过' % (anchor, len(hit)))
        continue
    k = hit[0]
    lines[k] = rewrap(lines[k], old, new)
    print('OK 第 %d 行  %s  <- %s' % (k + 1, why, new))

out = '\r\n'.join(lines)
assert out.count('\r\n') == data.count('\r\n'), '行数变了，异常'
open(P, 'wb').write(out.encode('utf-8'))
print('已写回 pvz.c，CRLF=%d' % out.count('\r\n'))
