# -*- coding: utf-8 -*-
"""把**线上**浏览器里的世界层原始像素导出来，渲染成 PNG。
为什么需要它：截图里混着浏览器外壳（缩放、按键条、安全区），
用肉眼在截图上量坐标会一直算错。直接拿世界层字节流，
就能和本机 C 侧导出的 _web_out.raw 用同一套坐标比较。"""
import sys, base64
from playwright.sync_api import sync_playwright
URL = "https://poker-pfr.site/pvz/"
IP = "111.229.27.13"

with sync_playwright() as pw:
    b = pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s" % IP,
        "--ignore-certificate-errors"])
    p = b.new_context(viewport={"width":1194,"height":834}, device_scale_factor=1).new_page()
    logs=[]
    p.on("console", lambda m: logs.append("[%s] %s"%(m.type,m.text)))
    p.goto(URL, wait_until="domcontentloaded", timeout=90000)
    for i in range(80):
        if not p.eval_on_selector("#bootBtn","el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    p.wait_for_timeout(8000)

    info = p.evaluate("""() => {
        const M = window.Module;
        const ptr = M.ccall('platWebFrame','number',[]);
        const n = 2000*1300*4;
        const bytes = M.HEAPU8.subarray(ptr, ptr + n);
        let s = '';
        const CH = 0x8000;
        for (let i = 0; i < bytes.length; i += CH)
            s += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i+CH, bytes.length)));
        return {b64: btoa(s), glyph: window.PVZ.glyphCount()};
    }""")
    data = base64.b64decode(info["b64"])
    open("_live_world.raw", "wb").write(data)
    print("  已导出 _live_world.raw (%d 字节)，字形 %s 个" % (len(data), info["glyph"]))
    for l in logs[-14:]:
        print("   ", l[:170])
    b.close()
