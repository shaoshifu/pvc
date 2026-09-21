# -*- coding: utf-8 -*-
"""布局诊断：量真实几何，不靠看截图猜。"""
import sys
from playwright.sync_api import sync_playwright

URL = "https://poker-pfr.site/pvz/?debug=1"
IP = "111.229.27.13"

def probe(pw, mobile, w, h, label):
    b = pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s" % IP,
        "--ignore-certificate-errors"])
    ctx = b.new_context(viewport={"width": w, "height": h},
                        device_scale_factor=2 if mobile else 1,
                        is_mobile=mobile, has_touch=mobile)
    p = ctx.new_page()
    p.goto(URL, wait_until="domcontentloaded", timeout=90000)
    for i in range(72):
        if not p.eval_on_selector("#bootBtn", "el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    p.wait_for_timeout(6000)

    d = p.evaluate("""() => {
        const st = document.getElementById('stage');
        const c  = document.getElementById('screen');
        const sr = st.getBoundingClientRect(), cr = c.getBoundingClientRect();
        const cs = getComputedStyle(c), ss = getComputedStyle(st);
        const o = {
          win: window.innerWidth + 'x' + window.innerHeight,
          vv:  (window.visualViewport ? (Math.round(window.visualViewport.width)+'x'+Math.round(window.visualViewport.height)) : 'n/a'),
          stageRect: [Math.round(sr.left),Math.round(sr.top),Math.round(sr.width),Math.round(sr.height)],
          canvasRect:[Math.round(cr.left),Math.round(cr.top),Math.round(cr.width),Math.round(cr.height)],
          canvasAttr: c.width + 'x' + c.height,
          cssW: c.style.width, cssH: c.style.height,
          stageCss: 'client '+st.clientWidth+'x'+st.clientHeight,
          stagePos: ss.position, stagePad: ss.padding, stageBox: ss.boxSizing,
          canvasPos: cs.position, canvasMaxW: cs.maxWidth, canvasDisp: cs.display,
          stageOverflow: ss.overflow,
        };
        // 逐行扫描 canvas：找每行的"非黑像素占比"，定位顶部黑带
        const g = c.getContext('2d');
        const rows = [];
        for (let y = 0; y < 300; y += 10) {
            const dd = g.getImageData(0, y, c.width, 1).data;
            let dark = 0, n = 0;
            for (let i = 0; i < dd.length; i += 4*7) { n++; if (dd[i]+dd[i+1]+dd[i+2] < 30) dark++; }
            rows.push(Math.round(y/c.height*1000)/10 + '%:' + Math.round(dark/n*100));
        }
        o.topRows = rows.join(' ');
        return o;
    }""")
    print("  ── %s (%dx%d, mobile=%s) ──" % (label, w, h, mobile))
    for k, v in d.items():
        print("     %-13s %s" % (k, v))
    p.screenshot(path="_diag_%s.png" % label)
    b.close()

with sync_playwright() as pw:
    probe(pw, True,  1194, 834, "ipad")
    probe(pw, False, 1440, 900, "desktop")
