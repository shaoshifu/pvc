# -*- coding: utf-8 -*-
"""进入游戏内画面，导出世界层确认卡片栏/阳光/波次文字都正常。"""
import base64
from playwright.sync_api import sync_playwright
URL="https://poker-pfr.site/pvz/"; IP="111.229.27.13"
with sync_playwright() as pw:
    b=pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s"%IP,"--ignore-certificate-errors"])
    ctx=b.new_context(viewport={"width":1194,"height":834},device_scale_factor=1,is_mobile=True,has_touch=True)
    p=ctx.new_page(); logs=[]
    p.on("console",lambda m: logs.append("[%s] %s"%(m.type,m.text)))
    p.goto(URL,wait_until="domcontentloaded",timeout=90000)
    for i in range(80):
        if not p.eval_on_selector("#bootBtn","el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn"); p.wait_for_timeout(4000)
    # 「开始游戏」按钮在逻辑坐标约 (180, 355)；换算到客户端坐标
    geo = p.evaluate("""() => {
        const st=document.getElementById('stage'), c=document.getElementById('screen');
        const sr=st.getBoundingClientRect(), cr=c.getBoundingClientRect();
        return {sx:sr.left, sy:sr.top, cx:cr.left, cy:cr.top, cw:cr.width, ch:cr.height};
    }""")
    sx = geo["cx"] + geo["cw"]*(180/1000.0)
    sy = geo["cy"] + geo["ch"]*(355/650.0)
    print("  点击开始游戏 @ 客户端 (%.0f, %.0f)" % (sx, sy))
    p.mouse.click(sx, sy)
    p.wait_for_timeout(6000)
    info = p.evaluate("""() => {
        const M=window.Module;
        const ptr=M.ccall('platWebFrame','number',[]);
        const n=2000*1300*4;
        const bytes=M.HEAPU8.subarray(ptr,ptr+n);
        let s=''; const CH=0x8000;
        for(let i=0;i<bytes.length;i+=CH) s+=String.fromCharCode.apply(null,bytes.subarray(i,Math.min(i+CH,bytes.length)));
        let big=0; const h=new Map();
        const u=M.HEAPU32.subarray(ptr>>2,(ptr>>2)+2000*1300);
        for(let i=0;i<u.length;i++){const v=u[i]&0xFFFFFF;h.set(v,(h.get(v)||0)+1);}
        for(const c of h.values()) if(c>1500) big++;
        return {b64:btoa(s), glyph: window.PVZ.glyphCount(), blocks: big,
                art: M.ccall('gameArtCount','number',[])};
    }""")
    open("_ingame.raw","wb").write(base64.b64decode(info["b64"]))
    print("  字形 %s / 纯色块 %s / 素材 %s" % (info["glyph"], info["blocks"], info["art"]))
    for l in logs[-8:]: print("   ", l[:170])
    b.close()
