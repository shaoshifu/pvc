# -*- coding: utf-8 -*-
"""同一次运行内，对比「wasm 世界层」与「canvas 实际像素」。

为什么必须做这个：我之前的验证只看了 wasm 世界层，**跳过了 shell.js 的像素转换**。
若两者不一致，问题在 shell；若一致，说明世界层里本来就是这样 —— 问题在引擎。
一次就能定死，不用猜。

用**桌面视口**（和用户截图一致的形态），不用 is_mobile。
"""
import sys, base64
from playwright.sync_api import sync_playwright

URL = "https://poker-pfr.site/pvz/"
IP  = "111.229.27.13"
W, H = 1088, 589          # 接近用户截图

with sync_playwright() as pw:
    b = pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s" % IP,
        "--ignore-certificate-errors"])
    p = b.new_context(viewport={"width": W, "height": H}, device_scale_factor=1).new_page()
    logs = []
    p.on("console", lambda m: logs.append("[%s] %s" % (m.type, m.text)))
    p.on("pageerror", lambda e: logs.append("PAGEERROR: %s" % e))
    p.goto(URL, wait_until="domcontentloaded", timeout=90000)
    for i in range(80):
        if not p.eval_on_selector("#bootBtn", "el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    p.wait_for_timeout(9000)

    d = p.evaluate("""() => {
        const M = window.Module;
        const c = document.getElementById('screen');
        const st = document.getElementById('stage');
        const g = c.getContext('2d');
        const canvasPx = g.getImageData(0, 0, c.width, c.height);

        // 世界层原始字节
        const ptr = M.ccall('platWebFrame','number',[]);
        const n = c.width * c.height;
        const world = M.HEAPU32.subarray(ptr>>2, (ptr>>2) + n);

        // 逐像素比较：canvas 是 RGBA，world 是 BGRA
        let diff = 0, total = 0, firstDiffs = [];
        for (let i = 0; i < n; i++) {
            const wp = world[i];
            const wR = (wp >> 16) & 255, wG = (wp >> 8) & 255, wB = wp & 255;
            const o = i * 4;
            const cR = canvasPx.data[o], cG = canvasPx.data[o+1],
                  cB = canvasPx.data[o+2], cA = canvasPx.data[o+3];
            total++;
            if (wR !== cR || wG !== cG || wB !== cB || cA !== 255) {
                diff++;
                if (firstDiffs.length < 5) {
                    const x = i % c.width, y = (i / c.width) | 0;
                    firstDiffs.push({x, y, world:[wR,wG,wB], canvas:[cR,cG,cB,cA]});
                }
            }
        }

        // 顺便把世界层导出，供肉眼比对
        const bytes = M.HEAPU8.subarray(ptr, ptr + n*4);
        let s = ''; const CH = 0x8000;
        for (let i = 0; i < bytes.length; i += CH)
            s += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i+CH, bytes.length)));

        const sr = st.getBoundingClientRect(), cr = c.getBoundingClientRect();
        return {
            b64: btoa(s),
            canvasAttr: c.width + 'x' + c.height,
            canvasCss: c.style.width + ' ' + c.style.height,
            canvasRect: [Math.round(cr.left), Math.round(cr.top), Math.round(cr.width), Math.round(cr.height)],
            stageRect: [Math.round(sr.left), Math.round(sr.top), Math.round(sr.width), Math.round(sr.height)],
            diff: diff, total: total, firstDiffs: firstDiffs,
            glyph: window.PVZ.glyphCount(),
            art: M.ccall('gameArtCount','number',[]),
        };
    }""")
    open("_cmp_world.raw","wb").write(base64.b64decode(d.pop("b64")))
    for k, v in d.items():
        print("  %-12s %s" % (k, v))
    print("\n  控制台:")
    for l in logs[-10:]: print("   ", l[:170])
    p.screenshot(path="_cmp_canvas.png")
    b.close()
