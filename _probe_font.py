# -*- coding: utf-8 -*-
"""探测浏览器里 Canvas2D 的字体能力：汉字能不能真的画出轮廓。
如果汉字渲染成"实心方块"（tofu / 缺字体），那么无论 C 侧多正确，
画面上都只会是一排方块 —— 这必须与"字形管线坏了"区分开。"""
from playwright.sync_api import sync_playwright

with sync_playwright() as pw:
    b = pw.chromium.launch(args=["--no-sandbox"])
    p = b.new_context(viewport={"width":800,"height":600}).new_page()
    p.goto("about:blank")
    d = p.evaluate("""() => {
        const FONT = '"Microsoft YaHei", "PingFang SC", "Hiragino Sans GB", "Heiti SC", "WenQuanYi Micro Hei", sans-serif';
        const cv = document.createElement('canvas');
        const g = cv.getContext('2d', {willReadFrequently:true});
        function probe(ch, px, font) {
            cv.width = px*2; cv.height = px*2;
            g.font = px + 'px ' + (font || FONT);
            g.textBaseline = 'alphabetic'; g.textAlign = 'left';
            g.clearRect(0,0,cv.width,cv.height);
            g.fillStyle = '#fff';
            g.fillText(ch, 2, px);
            const d = g.getImageData(0,0,cv.width,cv.height).data;
            let zero=0, full=0, mid=0, box=null;
            for (let i=0;i<d.length;i+=4){
                const a=d[i+3];
                if(a===0) zero++; else if(a===255) full++; else mid++;
                if(a>0){ const x=(i/4)%cv.width, y=Math.floor((i/4)/cv.width);
                    if(!box) box=[x,y,x,y];
                    else { box[0]=Math.min(box[0],x); box[1]=Math.min(box[1],y);
                           box[2]=Math.max(box[2],x); box[3]=Math.max(box[3],y); }
                }
            }
            return {zero, full, mid, box, inked:(full+mid),
                    ratio:+( (full)/((full+mid)||1) ).toFixed(3)};
        }
        const out = {};
        out.CJK_zhi   = probe('\\u690D', 72);
        out.CJK_wu    = probe('\\u7269', 72);
        out.Latin_A   = probe('A', 72);
        out.Digit_4   = probe('4', 72);
        out.Missing_fake = probe('\\uE000', 72);   /* 私用区，必然没有字形 */
        /* 系统里到底有哪些字体（能装上的都列出来） */
        try { out.fonts = Array.from(document.fonts).map(f=>f.family); } catch(e){ out.fonts='n/a'; }
        return out;
    }""")
    for k,v in d.items():
        print("  %-16s %s" % (k, v))
    b.close()
