# -*- coding: utf-8 -*-
"""诊断：页面里到底哪一环断了。直接读 wasm 侧的状态，不靠猜。"""
import sys
from playwright.sync_api import sync_playwright
URL = "https://poker-pfr.site/pvz/"
IP  = "111.229.27.13"

with sync_playwright() as pw:
    b = pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s" % IP,
        "--ignore-certificate-errors"])
    p = b.new_context(viewport={"width":1194,"height":834}, device_scale_factor=1).new_page()
    logs=[]; errs=[]
    p.on("console", lambda m: logs.append("[%s] %s"%(m.type,m.text)))
    p.on("pageerror", lambda e: errs.append(str(e)))
    p.goto(URL, wait_until="domcontentloaded", timeout=90000)
    for i in range(70):
        if not p.eval_on_selector("#bootBtn","el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    p.wait_for_timeout(5000)

    print("=== 页面内诊断 ===")
    d = p.evaluate("""() => {
        const M = window.Module;
        const out = {};
        out.hasModule = !!M;
        out.hasCCall  = !!(M && M.ccall);
        out.hasHEAPU32 = !!(M && M.HEAPU32);
        out.heapLen = (M && M.HEAPU32) ? M.HEAPU32.length : -1;
        try { out.artCount = M.ccall('gameArtCount','number',[]); } catch(e){ out.artCount='ERR:'+e; }
        try { out.stateId = M.ccall('gameStateId','number',[]); } catch(e){ out.stateId='ERR:'+e; }
        try { out.framePtr = M.ccall('platWebFrame','number',[]); } catch(e){ out.framePtr='ERR:'+e; }
        // 直接看世界层前几个像素（BGRA）
        try {
            const ptr = M.ccall('platWebFrame','number',[]);
            if (ptr) {
                const u = M.HEAPU32.subarray(ptr>>2, (ptr>>2)+8);
                out.firstPixels = Array.from(u).map(x=>'0x'+x.toString(16));
            } else out.firstPixels='ptr=NULL';
        } catch(e){ out.firstPixels='ERR:'+e; }
        const c = document.getElementById('screen');
        out.canvas = c.width+'x'+c.height;
        try {
            const dd = c.getContext('2d').getImageData(100,100,4,1).data;
            out.canvasPx = Array.from(dd);
        } catch(e){ out.canvasPx='ERR:'+e; }
        return out;
    }""")
    for k,v in d.items(): print("  %-14s %s" % (k, v))

    print("\n=== 控制台尾部（含 pvz.c 的 printf）===")
    for l in logs[-40:]: print("  ", l[:170])
    print("\n=== JS 异常 ===")
    for e in errs[:6]: print("  ", e[:200])
    b.close()
