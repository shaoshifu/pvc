# -*- coding: utf-8 -*-
import sys
from playwright.sync_api import sync_playwright
URL = "https://poker-pfr.site/pvz/?debug=1"
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
    p.wait_for_timeout(6000)

    print("=== ① MEMFS 里到底有没有资源 ===")
    fs = p.evaluate("""() => {
        const M = window.Module, o = {};
        try { o.cwd = M.FS.cwd(); } catch(e){ o.cwd='ERR '+e; }
        try { o.root = M.FS.readdir('/').slice(0,20); } catch(e){ o.root='ERR '+e; }
        try { o.hasAssets = M.FS.analyzePath('/assets').exists; } catch(e){ o.hasAssets='ERR '+e; }
        try {
            const l = M.FS.readdir('/assets');
            o.assetCount = l.length;
            o.assetSample = l.slice(0,6);
        } catch(e){ o.assetCount='ERR '+e; }
        try {
            const st = M.FS.stat('/assets/rgplant_00.png');
            o.rgplantSize = st.size;
        } catch(e){ o.rgplantSize='ERR '+String(e).slice(0,90); }
        return o;
    }""")
    for k,v in fs.items(): print("  %-14s %s" % (k,v))

    print("\n=== ② 有没有 platWebInit / 屏幕 DC 状态 ===")
    st = p.evaluate("""() => {
        const M = window.Module, o = {};
        o.hasPlatWebInit = typeof M.ccall === 'function';
        try { o.initRet = M.ccall('platWebInit','number',['number','number','string'],[1000,650,'x']); }
        catch(e){ o.initRet = 'ERR:'+String(e).slice(0,120); }
        return o;
    }""")
    for k,v in st.items(): print("  %-16s %s" % (k,v))

    print("\n=== ③ 引擎 console 输出（pvz.c 的 printf / jsLog）===")
    for l in logs:
        if 'plat_web' in l or 'err' in l.lower() or '素材' in l or 'init' in l.lower():
            print("  ", l[:170])
    print("  （console 总条数 %d）" % len(logs))
    print("\n=== ④ 页面诊断条文字（#diag）===")
    print("  ", p.inner_text("#diag")[:300].replace("\n"," | "))
    print("\n=== ⑤ JS 异常 ===")
    for e in errs[:6]: print("  ", e[:200])
    b.close()
