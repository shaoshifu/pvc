# -*- coding: utf-8 -*-
"""肉鸽单位贴图的公共工具：读 BMP / 抠图 / 内容裁切 / 贴到统一画布。

为什么单独成库（而不是塞进 _matte.py）：
  1. 生成单位贴图是 100 张的批处理，需要「读回已生成的 BMP 量一下内容多大」
     —— 这依赖 BMP 的原始字节解析，PIL 那套不能用（见下）。
  2. 游戏中所有精灵共用一套"画布 = 逻辑尺寸 × SS、底边贴地、按 drawing 缩放"
     的约定；单位贴图必须裁掉四周空白再居中贴到固定画布，
     否则同样大小的两只僵尸会因为出图留白不同而看起来一大一小。

⚠️ PIL 不能用来量 BMP 的 alpha：
   32 位 BMP 是 BI_RGB，规范里 alpha 字节「未定义」，PIL 会一律读成 255。
   游戏的 spriteLoad() 是直接按字节取的，所以只有按字节读才和游戏一致。
"""
import os
import struct

import numpy as np
from PIL import Image

SS = 2          # 游戏世界层超采样倍数：1 逻辑像素 = 2×2 资源像素
CELL_W = 96
CELL_H = 100


# ----------------------------------------------------------------- 读写
def read_bmp32(path):
    """按字节读 32 位 BMP，返回 (RGBA uint8, w, h)。自下而上的行序会被翻正。"""
    b = open(path, "rb").read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    if w <= 0 or abs(h) <= 0:
        raise ValueError("bad bmp size %s" % path)
    top_down = h < 0
    a = np.frombuffer(b[off:off + abs(h) * w * 4], np.uint8).reshape(abs(h), w, 4)
    a = a.reshape(abs(h), w, 4)[:, :, [2, 1, 0, 3]].copy()   # BGRA -> RGBA
    if not top_down:
        a = a[::-1]
    return a, w, abs(h)


def save_bmp32(rgba, path):
    """写 32 位 BMP。必须自下而上（biHeight = +h），游戏的 spriteLoad() 拒绝 h <= 0。"""
    a = np.asarray(rgba)
    h, w = a.shape[:2]
    bgra = a[::-1, :, [2, 1, 0, 3]]
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + w * h * 4, 0, 0, 14 + 40)
    ih = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, w * h * 4, 2835, 2835, 0, 0)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(ih)
        f.write(bgra.tobytes())


# ----------------------------------------------------------------- 抠图
def border_color(rgb, ring=6):
    """取边框一圈像素的中位色 = 背景色。返回 (color, 离散度)。

    为什么不写死"洋红"：出图模型在暗色主题的提示词下（酸蚀/诅咒/刺客）
    经常不听话，把背景画成暗红、深紫、灰黑。写死洋红的键控在这些图上
    会整张判成"不透明"，等于没抠。按边框取色则对任何纯色背景都成立。

    离散度用来判断背景是否真的均匀：偏大说明模型画了渐变/暗角/场景背景，
    这张图就不该硬抠，应该重生成（见 _audit_units.py）。
    """
    h, w = rgb.shape[:2]
    px = np.concatenate([rgb[:ring].reshape(-1, 3), rgb[-ring:].reshape(-1, 3),
                         rgb[:, :ring].reshape(-1, 3), rgb[:, -ring:].reshape(-1, 3)])
    c = np.median(px, axis=0)
    spread = float(np.mean(np.abs(px - c)))
    return c, spread


def key_out(rgb, c, lo=26.0, hi=100.0):
    """通用纯色键控 + 反预乘去边。

    alpha：到背景色的欧氏距离归一化 —— 背景 0、主体 1、边缘连续过渡，
           所以不会有硬锯齿。
    去边：边缘像素其实是「主体色 over 背景色」的合成结果。
          知道背景色 C 和 alpha 之后可以直接反解 P_true = (P-(1-a)C)/a，
          这在数学上是精确的，比"把某通道拉低"那种启发式干净得多，
          而且和背景是什么颜色无关（洋红、暗红、灰都行）。

    反预乘对很小的 alpha 会放大噪声，所以除数的下限取 0.18，
    并且 alpha < 0.06 直接判为全透明。
    """
    d = np.sqrt(((rgb - c) ** 2).sum(axis=2))
    a = np.clip((d - lo) / (hi - lo), 0.0, 1.0)
    a3 = a[:, :, None]
    out = (rgb - (1.0 - a3) * c) / np.maximum(a3, 0.18)
    out = np.clip(out, 0, 255)
    al = np.where(a < 0.06, 0.0, a * 255.0)
    return out, al


def flood_bg(rgb, tol=14.0, work=320):
    """从四条边框向内泛洪，判定哪些像素属于「连通的背景」。

    为什么需要它（单色键控不够用的场景）：
      提示词带「量子 / 能量 / 酸蚀」这类主题时，模型会画出**径向渐变背景**
      （边框是深紫、中心是亮粉）。到边框中位色的距离在中心会很大，
      单色键控会把渐变中心判成不透明 —— 于是角色背后糊一大块粉。
      泛洪判定的是「和邻近背景像素是否接近」，颜色一路渐变过去也能连通，
      所以整片渐变背景都能被吃掉，而角色的硬边（局部色差大）会挡住泛洪。

    纯 numpy 实现（环境里没有 scipy / cv2）：
      迭代膨胀一圈，只保留「与已确定背景的局部色差 < tol」的新像素。
      渐变背景每格变化远小于 tol -> 一路传播；角色轮廓处色差突变 -> 停下。

    性能：直接在 1024px 上做需要数百轮全图计算，太慢。
    这里缩到 work 边长的小图上泛洪，再线性放回原尺寸 ——
    背景是低频的（渐变），缩图上的连通性和原图一致，而计算量降两个数量级。
    """
    h, w = rgb.shape[:2]
    im = Image.fromarray(rgb.astype(np.uint8), "RGB")
    k = min(1.0, float(work) / max(h, w))
    sw, sh = max(16, int(w * k)), max(16, int(h * k))
    small = np.array(im.resize((sw, sh), Image.BILINEAR)).astype(np.float32)

    c, spread = border_color(small)
    tol = max(tol, spread * 2.2)

    def grow(m):
        out = m.copy()
        out[1:, :] |= m[:-1, :]
        out[:-1, :] |= m[1:, :]
        out[:, 1:] |= m[:, :-1]
        out[:, :-1] |= m[:, 1:]
        return out

    mask = np.zeros((sh, sw), bool)
    mask[0, :] = mask[-1, :] = mask[:, 0] = mask[:, -1] = True
    # 已知背景的"当前色"：用掩膜加权模糊，让参考色跟随渐变推进
    known = np.where(mask[:, :, None], small, 0.0)
    wgt = mask.astype(np.float32)[:, :, None]

    for _ in range(max(sh, sw)):
        cand = grow(mask) & ~mask
        if not cand.any():
            break
        ref = _blur3(known) / np.maximum(_blur3(wgt), 1e-3)
        d = np.sqrt(((small - ref) ** 2).sum(axis=2))
        add = cand & (d < tol)
        if not add.any():
            break
        mask |= add
        known[add] = small[add]
        wgt[add] = 1.0

    return np.array(Image.fromarray((mask * 255).astype(np.uint8)).resize(
        (w, h), Image.BILINEAR)).astype(np.float32) / 255.0 > 0.5


def _blur3(a):
    """3x3 均值模糊（same 卷积，移位相加实现，纯 numpy）。"""
    out = np.zeros_like(a, np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            out += np.roll(np.roll(a, dy, 0), dx, 1)
    return out / 9.0


def matte_file(path, lo=26.0, hi=100.0, flood=True):
    """读 JPG/PNG -> RGBA，自动识别背景色抠掉（顺带清掉烘焙的地面阴影）。

    两条路径：
      flood=True（默认）：先泛洪定出「背景区域」，再用距离渐变算 α。
        能处理径向渐变的背景（模型在能量系主题下常画），
        这是 100 张批处理里唯一稳住的方法。
      flood=False：纯单色键控，背景必须是均匀纯色。

    ⚠️ 为什么泛洪结果要加 `al < 200` 这道保护 —— 试过不加，不行：
        `np.where(bg, 0, al)`（泛洪区内一律清零）看着更"彻底"，实测会把角色啃掉。
        P26 是渐变洋红背景（边框离散 16.1），为了吃透渐变，泛洪容差被抬到
        tol = spread*2.2 ≈ 35；而蘑菇帽自身的蓝色也是平滑渐变，同样满足这个容差，
        于是波前顺着帽子爬进角色内部 —— 不透明率 40% -> 11%，整只蘑菇被挖空
        （见 art/panels_P26.png 第 4 面板）。

    也试过"边缘屏障泛洪"（用梯度幅值当屏障，让泛洪在角色轮廓处停下）
    + "背景色场 + 受限剥离"（向颜色接近背景色的方向逐圈剥离光晕/颗粒）：
    前者能保住角色但吃不掉贴轮廓的光晕与零散颗粒；
    后者剥离圈数一多，把软边角色（P33 的脸）也一并吃掉（不透明 61% -> 10%）。

    结论：算法层面在"吃掉渐变背景"和"保住软边角色"之间没有稳定解，
    所以保留这道保护，把**贴轮廓的粉雾**归为源图缺陷，用重生成解决
    （art/_gen_units.py --strict --bestof）。
    """
    im = Image.open(path).convert("RGB")
    a = np.array(im).astype(np.float32)
    c, spread = border_color(a)
    rgb2, al = key_out(a, c, lo, hi)
    if flood:
        bg = flood_bg(a)
        # 泛洪认定是背景的地方强压到透明；其余保持键控结果。
        # 只在"键控也偏透明"的地方压，避免把贴着背景色的角色暗部（以及
        # 角色自身的平滑渐变）吃掉 —— 理由见上面函数注释里的实测数据。
        al = np.where(bg & (al < 200), 0.0, al)
    out = np.dstack([rgb2, al]).astype(np.uint8)
    return Image.fromarray(out, "RGBA"), c, spread


# ----------------------------------------------------------------- 裁切贴合
def content_bbox(im, thr=24):
    a = np.array(im)[:, :, 3]
    ys, xs = np.where(a > thr)
    if len(xs) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def fit_to_canvas(im, canvas_w, canvas_h, pad_x=0.0, pad_top=0.0, align="bottom"):
    """裁掉透明边 -> 等比缩放到画布内 -> 水平居中 / 底边贴齐。

    pad_x / pad_top 是不缩放的固定内缩（资源像素）。
    """
    bb = content_bbox(im)
    if bb is None:
        return Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    x0, y0, x1, y1 = bb
    sub = im.crop((x0, y0, x1 + 1, y1 + 1))

    aw = max(8, canvas_w - int(pad_x * 2))
    ah = max(8, canvas_h - int(pad_top))
    k = min(aw / sub.width, ah / sub.height)
    nw = max(1, int(round(sub.width * k)))
    nh = max(1, int(round(sub.height * k)))
    sub = sub.resize((nw, nh), Image.LANCZOS)

    out = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    ox = (canvas_w - nw) // 2
    oy = (canvas_h - nh) if align == "bottom" else (canvas_h - nh) // 2
    out.alpha_composite(sub, (ox, oy))
    return out


def stat(im):
    a = np.array(im)
    al = a[:, :, 3]
    return {
        "size": "%dx%d" % (im.width, im.height),
        "opaque%": round(float((al > 200).mean() * 100), 1),
        "clear%": round(float((al == 0).mean() * 100), 1),
        "edge_min": int(min(al[:3].min(), al[-3:].min(), al[:, :3].min(), al[:, -3:].min())),
    }
