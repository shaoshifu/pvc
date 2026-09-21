# -*- coding: utf-8 -*-
"""线上验证：菜单 UI 必须"顶住时间"，不能跑几十秒后只剩文字。
用桌面视口（和用户报障时一致），跑满 90 秒，多次采样菜单背景。"""
import base64
from playwright.sync_api import sync_playwright
URL="https://poker-pfr.site/pvz/"; IP="111.229.27.13"
with sync_playwright() as pw:
    b=pw.chromium.launch(args=["--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s"%IP,"--ignore-certificate-errors"])
    p=b.new_context(viewport={"width":1088,"height":589},device_scale_factor=1).new_page()
    p.goto(URL,wait_until="domcontentloaded",timeout=90000)
    for i in range(80):
        if not p.eval_on_selector("#bootBtn","el=>el.disabled"): break
        p.wait_for_timeout(5000)
    p.click("#bootBtn")
    print("  时间   菜单背景采样(world 200,60)  纯色块  颜色种类  字样")
    samples=[]
    for t in (2, 10, 30, 60, 90):
        p.wait_for_timeout((t - (samples[-1][0] if samples else 0))*1000)
        d=p.evaluate("""() => {
            const M=window.Module, ptr=M.ccall('platWebFrame','number',[]);
            const w=2000,h=1300, u=M.HEAPU32.subarray(ptr>>2,(ptr>>2)+w*h);
            const px=u[60*w+200]&0xFFFFFF;
            const hist=new Map();
            for(let i=0;i<u.length;i++){const v=u[i]&0xFFFFFF;hist.set(v,(hist.get(v)||0)+1);}
            let big=0; for(const c of hist.values()) if(c>1500) big++;
            return {px, blocks:big, colors:hist.size, glyph:window.PVZ.glyphCount()};
        }""")
        samples.append((t, d))
        print("  %3ds    0x%06X                   %-5s  %-8s  %s" %
              (t, d["px"], d["blocks"], d["colors"], d["glyph"]))
    p.screenshot(path="_verify_live.png")
    from PIL import Image
    im=Image.open("_verify_live.png").convert("RGB")
    im.save("_verify_live.jpg", quality=92)
    px0=samples[0][1]["px"]
    ok = all(s[1]["px"]==px0 for s in samples)
    print("\n  菜单背景 90 秒内是否恒定: %s" % ("OK（修复生效）" if ok else "★ FAIL（仍在翻转）"))
    print("  最终纯色块数: %d（UI 底板+文字块，应 >= 8）" % samples[-1][1]["blocks"])
    b.close()
