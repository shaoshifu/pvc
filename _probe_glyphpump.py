# -*- coding: utf-8 -*-
"""带 ?debug=1 跑，把控制台**原样**打出来，并跟踪 glyphCount 随时间的增长。
目的：找出"字形只灌进去一部分"是被什么挡住的。"""
from playwright.sync_api import sync_playwright
URL="https://poker-pfr.site/pvz/?debug=1"; IP="111.229.27.13"
with sync_playwright() as pw:
    b=pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s"%IP,"--ignore-certificate-errors"])
    p=b.new_context(viewport={"width":1194,"height":834},device_scale_factor=1).new_page()
    logs=[]
    p.on("console",lambda m: logs.append("[%s] %s"%(m.type,m.text)))
    p.on("pageerror",lambda e: logs.append("PAGEERROR: %s"%e))
    p.goto(URL,wait_until="domcontentloaded",timeout=90000)
    for i in range(80):
        if not p.eval_on_selector("#bootBtn","el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    print("  glyphCount 随时间:")
    for i in range(12):
        p.wait_for_timeout(1000)
        try:
            n=p.evaluate("()=>window.PVZ.glyphCount()")
            req=p.evaluate("()=>{try{return Module.ccall('platWebGlyphReq','number',[0,0,0]);}catch(e){return 'ERR';}}")
            print("    [%2ds] glyph=%s  reqPending=%s" % (i+1, n, req))
        except Exception as ex:
            print("    [%2ds] 读取失败 %s" % (i+1, str(ex)[:80]))
    print("\n  控制台输出（原样，最后 30 条）:")
    for l in logs[-30:]:
        print("    ", l[:200])
    b.close()
