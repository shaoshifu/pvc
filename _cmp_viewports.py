# -*- coding: utf-8 -*-
"""同一份代码、三个视口，各自导出世界层，看画面是否随视口变化。

为什么这件事很关键：我"看起来正常"的那次是 1194x834，
用户看到问题的形态是桌面视口。如果画面随视口变化，
说明有东西在依赖视口大小 —— 而 canvas 的像素尺寸始终是 2000x1300，
**理论上不该有影响**。找到这个依赖就是根因。
"""
import sys, base64
from playwright.sync_api import sync_playwright

URL = "https://poker-pfr.site/pvz/"
IP  = "111.229.27.13"
VIEWPORTS = [(1194, 834, "a_1194x834"), (1088, 589, "b_1088x589"), (1440, 900, "c_1440x900")]

with sync_playwright() as pw:
    b = pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s" % IP,
        "--ignore-certificate-errors"])
    for vw, vh, tag in VIEWPORTS:
        ctx = b.new_context(viewport={"width": vw, "height": vh}, device_scale_factor=1)
        p = ctx.new_page()
        logs = []
        p.on("console", lambda m: logs.append("[%s] %s" % (m.type, m.text)))
        p.goto(URL, wait_until="domcontentloaded", timeout=90000)
        for i in range(80):
            if not p.eval_on_selector("#bootBtn", "el=>el.disabled"): break
            p.wait_for_timeout(5000)
        p.click("#bootBtn")
        p.wait_for_timeout(9000)

        d = p.evaluate("""() => {
            const M = window.Module;
            const c = document.getElementById('screen');
            const n = 2000*1300;
            const ptr = M.ccall('platWebFrame','number',[]);
            const bytes = M.HEAPU8.subarray(ptr, ptr + n*4);
            let s=''; const CH=0x8000;
            for (let i=0;i<bytes.length;i+=CH)
                s += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i+CH, bytes.length)));
            // 画面统计：颜色种类 + 纯色块数（UI 底板会形成大块纯色）
            const u = M.HEAPU32.subarray(ptr>>2, (ptr>>2)+n);
            const h = new Map();
            for (let i=0;i<u.length;i++){const v=u[i]&0xFFFFFF;h.set(v,(h.get(v)||0)+1);}
            let big=0; for (const cnt of h.values()) if (cnt>1500) big++;
            return {b64: btoa(s), colors: h.size, blocks: big,
                    css: c.style.width+' '+c.style.height,
                    gym: window.PVZ.glyphCount(),
                    art: M.ccall('gameArtCount','number',[])};
        }""")
        open("_vp_%s.raw" % tag, "wb").write(base64.b64decode(d.pop("b64")))
        print("  %-12s css=%-16s 颜色种类=%-7s 纯色块=%-5s 字形=%-5s 素材=%s"
              % (tag, d["css"], d["colors"], d["blocks"], d["gym"], d["art"]))
        for l in logs[-4:]:
            print("       ", l[:150])
        p.screenshot(path="_vp_%s.png" % tag)
        ctx.close()
    b.close()
