# -*- coding: utf-8 -*-
"""字形诊断：确认 platWebGlyph 是否真的注入成功、以及文字有没有被画到世界层。"""
from playwright.sync_api import sync_playwright
URL = "https://poker-pfr.site/pvz/"
IP = "111.229.27.13"

with sync_playwright() as pw:
    b = pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s" % IP,
        "--ignore-certificate-errors"])
    p = b.new_context(viewport={"width":1194,"height":834}, device_scale_factor=1).new_page()
    alllogs = []
    p.on("console", lambda m: alllogs.append("[%s] %s" % (m.type, m.text)))
    p.on("pageerror", lambda e: alllogs.append("PAGEERROR: %s" % e))
    p.goto(URL, wait_until="domcontentloaded", timeout=90000)
    for i in range(80):
        if not p.eval_on_selector("#bootBtn","el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    p.wait_for_timeout(6000)

    print("=== wasm 侧字形数 vs 外壳计数 ===")
    d = p.evaluate("""() => {
        const M = window.Module, o = {};
        o.shellCount = window.PVZ.glyphCount();
        try { o.wasmCount = M.ccall('platWebGlyphCount','number',[]); } catch(e){ o.wasmCount='ERR:'+e; }
        try { o.hasHEAP32 = typeof M.HEAP32; } catch(e){ o.hasHEAP32='ERR'; }
        try { o.hasMalloc = typeof M._malloc; } catch(e){ o.hasMalloc='ERR'; }
        try { o.reqLeft = M.ccall('platWebGlyphReq','number',[0,0,0]); } catch(e){ o.reqLeft='ERR:'+e; }
        /* 手工再灌一个明显的字形，看 wasm 计数会不会涨 —— 验证 platWebGlyph 可调用 */
        try {
            const cp = 0x4E2D;             // 中
            const w = 4, h = 30, adv = 32;
            const buf = M._malloc(w*h);
            M.HEAPU8.fill(255, buf, buf + w*h);
            const r = M.ccall('platWebGlyph','number',
                ['number','number','number','number','number','number','number'],
                [cp, 30, 0, buf, w, h, adv]);
            M._free(buf);
            o.testInjectRet = r;
            o.wasmCountAfter = M.ccall('platWebGlyphCount','number',[]);
        } catch(e){ o.testInjectRet = 'ERR:'+e; }
        /* 统计世界层顶部左侧（卡片区）与阳光数字区的非黑像素 */
        try {
            const ptr = M.ccall('platWebFrame','number',[]);
            const W = 2000;
            function countNonBlack(x0,y0,x1,y1){
                let n=0,t=0;
                for (let y=y0;y<y1;y++) for (let x=x0;x<x1;x++){
                    const v = M.HEAPU32[(ptr>>2) + y*W + x];
                    if ((v & 0xFFFFFF) !== 0) n++;
                    t++;
                }
                return n + '/' + t;
            }
            o.sunTextArea   = countNonBlack(240, 40, 320, 100);    // 阳光数字（逻辑120-160,20-50）
            o.cardPriceArea = countNonBlack(300, 130, 420, 180);   // 第一张卡的价格行
            o.titleArea     = countNonBlack(0, 0, 2000, 60);       // 顶栏
        } catch(e){ o.frame = 'ERR:'+e; }
        return o;
    }""")
    for k, v in d.items():
        print("   %-18s %s" % (k, v))

    print("\n=== 全部控制台输出（不过滤）===")
    for l in alllogs[-26:]:
        print("   ", l[:190])
    b.close()
