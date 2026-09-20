# -*- coding: utf-8 -*-
"""机制完整性审计：静态检查每个能力位与状态字段是否真的接通了。

为什么需要静态审计（而不是只靠跑游戏看）：
  一个"死"机制在画面上**和没触发长得一模一样** —— 都不动。
  你没法用眼睛区分"这株植物的击退没生效"和"这株植物这波没打到人"。
  但 grep 能：如果某个 trait 位在整个源码里除了 `#define` 和
  数据表以外一次都没出现过，那它 100% 没实现，不需要跑。

检查项：
  A. 能力位覆盖 —— 每个 RG_T_* / RZ_* / RZ2_* 位，在"逻辑区"里是否至少有一处引用。
     只有声明没有引用 = 这株单位的能力是空的，只是个数值怪。
  B. 状态字段接通 —— Plant / Zombie 的每个字段，是否既被**写入**又被**读取**。
     · 只写不读 = 状态设了但没人消费（例如给僵尸写了 slow，但移动逻辑不看它）
     · 只读不写 = 永远读到初始值（例如判定 armSwing 却从没赋值）
  C. 别名一致性 —— `RG_T_XXX_SAFE = RG_T_XXX` 这批，查真名和别名都要能命中。

用法：
  python _audit_mechanics.py            # 全量报告
  python _audit_mechanics.py --brief    # 只看问题清单
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = open(os.path.join(ROOT, "pvz.c"), encoding="utf-8", errors="replace").read()


def logic_body():
    """去掉 `#define` 行与两个数据表之后的源码 —— 剩下的才叫"实现"。

    ⚠️ 第一版只去掉了 rgPlants，结果僵尸数据表里的 RZ_* 全被当成"有引用"，
    Z 系列审计全绿但其实是假的。两张表都要剥掉。
    """
    i0 = SRC.index("static const RgPlantDef rgPlants")
    body = SRC[:i0] + SRC[SRC.index("\n};", i0) + 3:]
    j0 = body.index("static const RgZombieDef rgZombies")
    body = body[:j0] + body[body.index("\n};", j0) + 3:]
    return re.sub(r"^#define\s+\w+.*$", "", body, flags=re.M)


KEYWORDS = {"int", "float", "unsigned", "char", "long", "COLORREF",
            "const", "wchar_t", "static", "struct"}


def struct_fields(name):
    """取某个 struct 的字段名列表。

    ⚠️ 不能用 `typedef struct \\{(.*?)\\n\\} Name;` 这种非贪婪匹配：
    `re.search` 会从**文件里第一个** `typedef struct {` 开始，
    一路非贪婪到目标的 `}`，把中间几十个结构体全吞进去
    —— 实测 Plant 因此报出 1690 个"字段"，整张表全是噪声。
    正确做法：先定位 `} Name;`，再向前找**最近的** `typedef struct`。
    """
    if ("} %s;" % name) not in SRC:
        return []
    end = SRC.index("} %s;" % name)
    start = SRC.rindex("typedef struct", 0, end)
    body = re.sub(r"/\*.*?\*/", "", SRC[start:end], flags=re.S)
    out = []
    for decl in body.split(";"):
        line = decl.strip().split("\n")[-1].strip()
        ids = re.findall(r"[A-Za-z_]\w*", line)
        for nm in ids[1:]:          # 第一个标识符是类型名
            if nm not in KEYWORDS:
                out.append(nm)
    return out


def field_usage(body, fname):
    """返回 (读次数, 写次数)。

    ⚠️ 必须同时覆盖 `->field` 和 `.field` 两种访问形式。
    第一版只查 `->field`，于是 `grid[row][col].summon = 1;` 这种
    **数组下标直接访问**被漏掉，summon 被误报成"只读不写（功能未启用）"。
    而这个项目里 grid[][] / zombies[] / peas[] 都是直接下标访问的，
    漏掉 `.field` 会让一半的字段判错。
    """
    acc = r"(?:->|\.)\s*" + fname + r"\b"
    write = len(re.findall(r"(?:->|\.)\s*" + fname + r"\s*(?:[-+*/]?=(?!=)|\+\+|--)", body))
    total = len(re.findall(acc, body))
    return max(0, total - write), write


def main():
    brief = "--brief" in sys.argv
    body = logic_body()

    aliases = {}
    for m in re.finditer(r"^#define\s+(RG_T_\w+|RZ\w*_\w+)\s+(RG_T_\w+|RZ\w*_\w+)\s*$", SRC, re.M):
        aliases.setdefault(m.group(2), []).append(m.group(1))

    groups = []
    for pat, label in ((r"^#define (RG_T_\w+)", "植物能力位"),
                       (r"^#define (RZ_\w+)", "僵尸能力位 A"),
                       (r"^#define (RZ2_\w+)", "僵尸能力位 B"),
                       (r"^#define (RZ3_\w+)", "僵尸能力位 C")):
        names = []
        for m in re.finditer(pat, SRC, re.M):
            n = m.group(1)
            if n.endswith("_SAFE") or n == "RZ2_ALL" or n == "RG_ALL_TRAITS":
                continue
            if n not in names:
                names.append(n)
        groups.append((label, names))

    print("# 机制完整性审计\n")
    print("源文件：pvz.c（%d 行，%d 字节）\n" % (SRC.count("\n") + 1, len(SRC)))

    all_missing = []
    for label, names in groups:
        missing = []
        for n in names:
            hit = (n in body) or any(a in body for a in aliases.get(n, []))
            if not hit:
                missing.append(n)
        all_missing += [(label, n) for n in missing]
        print("## %s：%d 位，已接通 %d，未接通 %d"
              % (label, len(names), len(names) - len(missing), len(missing)))
        if missing and not brief:
            print()
            for n in missing:
                print("  ✗ %s" % n)
        print()

    print("## 状态字段接通情况\n")
    for sname in ("Plant", "Zombie"):
        fields = struct_fields(sname)
        bad = []
        for f in fields:
            r, w = field_usage(body, f)
            if w == 0:
                bad.append((f, r, w, "只读不写（永远读到初始值）"))
            elif r == 0:
                bad.append((f, r, w, "只写不读（状态设了但没人消费）"))
        print("### %s（%d 个字段）" % (sname, len(fields)))
        if not bad:
            print("  ✔ 全部字段读写成对\n")
        else:
            for f, r, w, why in bad:
                print("  ✗ %-12s 读 %-3d 写 %-3d  %s" % (f, r, w, why))
            print()

    # ---- 影响面：把未接通的位映射回具体单位 ----
    # 光说"BOUNCE5 没实现"没用，得知道是哪些植物/僵尸在挂着这个空壳。
    def rows_of(tbl):
        i0 = SRC.index(tbl)
        i1 = SRC.index("\n};", i0)
        out = []
        for m in re.finditer(r"/\* (\d{2}) \*/ \{\s*L\"([^\"]+)\",\s*(RGQ_\w+),\s*(\w+),\s*([^,]+),",
                             SRC[i0:i1], re.S):
            out.append((m.group(1), m.group(2), m.group(5)))
        return out

    def impact(tbl, tag):
        hit = {}
        for idx, nm, tr in rows_of(tbl):
            for _, miss in all_missing:
                if re.search(r"\b" + miss + r"\b", tr):
                    hit.setdefault(miss, []).append("%s%s %s" % (tag, idx, nm))
        if hit:
            print("### %s\n" % tbl.split()[3])
            for k in sorted(hit):
                print("  · %-18s ← %s" % (k, "、".join(hit[k])))
            print("")

    if all_missing:
        print("## 影响面（谁挂着这些没实现的位）\n")
        impact("static const RgPlantDef rgPlants", "P")
        impact("static const RgZombieDef rgZombies", "Z")

    print("## 结论\n")
    if not all_missing:
        print("- 能力位：✔ 全部接通")
    else:
        print("- 能力位：✗ %d 位未接通" % len(all_missing))
        print("  " + "、".join(sorted(set(n for _, n in all_missing))))


if __name__ == "__main__":
    main()
