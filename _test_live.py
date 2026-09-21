# -*- coding: utf-8 -*-
"""线上站点端到端验证：真浏览器跑一遍，确认 WASM 真的能启动、素材真的载入、画面真的有内容。

【为什么必须做这一步（而不是只 curl 文件）】
  curl 只能证明"文件能下载"。网页版的失败模式大多是**运行期**的：
    · wasm 加载/实例化失败
    · pvz.data 读不出来（MEMFS 路径不对）
    · 初始化 JS 报错导致按钮永远不可点
  更阴的是 2026-09-21 那次：文件全都 200、wasm 正常启动、0 个 JS 异常、
  引导页正常走完 —— 但**画面全黑**。原因是 wsprintfW 的 %s 语义偏了，
  853 张素材一张没加载，而引擎只是安静地退化成程序化图形。
  也就是说：**没有异常不等于没坏**。所以这里断言的是具体的量：
    · gameArtCount() 必须等于 853（素材真的进了内存）
    · canvas 必须有内容（不是一片黑）
    · 世界层指针非空且像素非零（渲染管线真的在产出）

【DNS 绕行】
  本机 DNS 被沙箱拦截（域名解析到 198.18.x.x 保留段），
  所以用 Chromium 的 --host-resolver-rules 把域名映射到真实 IP。
  比改 hosts 干净：不影响本机其它程序、也不需要管理员权限。

用法：
  python _test_live.py                    # 默认测 https://poker-pfr.site/pvz/
  python _test_live.py https://域名/pvz/  # 指定地址
"""
import sys

from playwright.sync_api import sync_playwright

URL = sys.argv[1] if len(sys.argv) > 1 else "https://poker-pfr.site/pvz/"
REAL_IP = "111.229.27.13"
HOSTS = ["poker-pfr.site", "poker-cfr.site", "www.poker-pfr.site", "www.poker-cfr.site"]
EXPECT_ART = 853
EXPECT_GLYPH_MIN = 50      # 惰性光栅化：用多少灌多少，标题画面约 125

args = ["--no-sandbox", "--ignore-certificate-errors",
        "--host-resolver-rules=" + ",".join("MAP %s %s" % (h, REAL_IP) for h in HOSTS)]

with sync_playwright() as pw:
    browser = pw.chromium.launch(args=args)
    ctx = browser.new_context(
        viewport={"width": 1194, "height": 834},      # iPad Pro 11 横屏
        device_scale_factor=2,
        is_mobile=True, has_touch=True,
    )
    page = ctx.new_page()
    logs, errs, failed = [], [], []
    page.on("console", lambda m: logs.append("[%s] %s" % (m.type, m.text)))
    page.on("pageerror", lambda e: errs.append(str(e)))
    page.on("requestfailed", lambda r: failed.append("%s :: %s" % (r.url, r.failure)))

    print("(1) open %s  [DNS -> %s]" % (URL, REAL_IP))
    page.goto(URL, wait_until="domcontentloaded", timeout=90000)
    page.wait_for_timeout(2500)
    print("    title: %s" % page.title())

    print("(2) wait for boot button (needs the 76MB pvz.data)")
    ready = False
    for i in range(72):                                # up to 6 min
        try:
            dis = page.eval_on_selector("#bootBtn", "el => el.disabled")
        except Exception as ex:
            print("    bootBtn not found: %s" % str(ex)[:90])
            break
        if i % 8 == 0:
            msg = ""
            try:
                msg = page.inner_text("#bootMsg").strip()[:70]
            except Exception:
                pass
            print("    [%4ds] disabled=%s  msg=%r" % (i * 5, dis, msg))
        if not dis:
            ready = True
            break
        page.wait_for_timeout(5000)

    if not ready:
        print("    !! boot button never became clickable -> runtime init failed")
        for l in logs[-30:]:
            print("      ", l[:170])
        for x in errs[-8:]:
            print("       ERR:", x[:200])
        page.screenshot(path="_live_fail.png", full_page=True)
        browser.close()
        sys.exit(1)

    print("(3) click start")
    page.click("#bootBtn")
    page.wait_for_timeout(7000)                        # let it decode 853 sprites

    print("(4) read engine state from wasm")
    st = page.evaluate("""() => {
        const M = window.Module, o = {};
        o.hasModule = !!M;
        try { o.artCount = M.ccall('gameArtCount','number',[]); } catch(e){ o.artCount = 'ERR:'+e; }
        /* ★ 已注入的字形数。0 = 整屏没有任何文字，而这不抛异常、不失败请求，
           所以只能靠这个数字发现（2026-09-15 那轮就是漏了它，
           画面里"没有字"被当成"UI 没做"）。 */
        try { o.glyphCount = window.PVZ.glyphCount(); } catch(e){ o.glyphCount = 'ERR:'+e; }
        try { o.reqLeft = M.ccall('platWebGlyphReq','number',[0,0,0]); } catch(e){ o.reqLeft = 'ERR:'+e; }
        try {
            const ptr = M.ccall('platWebFrame','number',[]);
            o.framePtr = ptr;
            if (ptr) {
                const u = M.HEAPU32.subarray(ptr>>2, (ptr>>2)+16);
                o.nonZeroWords = Array.from(u).filter(x => x !== 0).length;
            }
        } catch(e){ o.framePtr = 'ERR:'+e; }
        /* ★ 画面里到底有没有「成块的文字」。
           字形注入成功 ≠ 文字被画出来（n<0 那个坑就是：注入了、没画）。
           做法：世界层里统计纯色块 —— 文字是覆盖率贴图，会形成大块同色区域；
           插画背景是连续渐变，不会。所以"存在 >2000 px 的纯色块"
           ⇔ "文字确实落在画面上"。 */
        try {
            const ptr = M.ccall('platWebFrame','number',[]);
            const n2 = 2000*1300;
            const u = M.HEAPU32.subarray(ptr>>2, (ptr>>2)+n2);
            const hist = new Map();
            for (let i = 0; i < n2; i++) {
                const v = u[i] & 0xFFFFFF;
                hist.set(v, (hist.get(v)||0) + 1);
            }
            let big = 0;
            for (const cnt of hist.values()) if (cnt > 2000) big++;
            o.textBlocks = big;
        } catch(e){ o.textBlocks = 'ERR:'+e; }
        try {
            const c = document.getElementById('screen');
            o.canvas = c.width + 'x' + c.height;
            const d = c.getContext('2d').getImageData(0,0,c.width,c.height).data;
            let r=0,g=0,b=0,n=0,dark=0; const distinct = new Set();
            for (let i=0;i<d.length;i+=4*503){
                r+=d[i]; g+=d[i+1]; b+=d[i+2]; n++;
                if (d[i]+d[i+1]+d[i+2] < 30) dark++;
                if (n < 4000) distinct.add((d[i]>>4)+','+(d[i+1]>>4)+','+(d[i+2]>>4));
            }
            o.avgRGB = [Math.round(r/n), Math.round(g/n), Math.round(b/n)];
            o.darkPct = Math.round(dark/n*100);
            o.distinctColors = distinct.size;
        } catch(e){ o.canvas = 'ERR:'+e; }
        return o;
    }""")
    for k, v in st.items():
        print("    %-16s %s" % (k, v))

    page.screenshot(path="_live_ipad.png")
    page.wait_for_timeout(1200)
    page.screenshot(path="_live_ipad2.png")

    # 再点一下屏幕，确认交互后画面仍在推进（不是静态一张图）
    try:
        page.mouse.click(600, 500)
        page.wait_for_timeout(1500)
        page.screenshot(path="_live_after_tap.png")
    except Exception:
        pass

    bad_logs = [l for l in logs
                if any(k in l.lower() for k in ("error", "fail", "404", "undefined",
                                                "not a function", "abort", "exception"))]
    print("(5) suspicious console lines: %d" % len(bad_logs))
    for l in bad_logs[:10]:
        print("      ", l[:170])
    print("(6) JS exceptions: %d" % len(errs))
    for x in errs[:5]:
        print("      ", x[:200])
    print("(7) failed requests: %d" % len(failed))
    for f in failed[:8]:
        print("      ", f[:180])

    browser.close()

    art_ok = st.get("artCount") == EXPECT_ART
    glyph_ok = isinstance(st.get("glyphCount"), int) and st["glyphCount"] >= EXPECT_GLYPH_MIN
    text_ok = isinstance(st.get("textBlocks"), int) and st["textBlocks"] >= 1
    frame_ok = isinstance(st.get("framePtr"), int) and st["framePtr"] != 0 and st.get("nonZeroWords", 0) > 0
    canvas_ok = (isinstance(st.get("avgRGB"), list) and st.get("darkPct", 100) < 60
                 and st.get("distinctColors", 0) > 8)

    print("")
    print("  art count      %s  (want %d)" % ("OK" if art_ok else "FAIL (%s)" % st.get("artCount"), EXPECT_ART))
    print("  glyph count    %s  (want >= %d)" % ("OK" if glyph_ok else "FAIL (%s)" % st.get("glyphCount"), EXPECT_GLYPH_MIN))
    print("  text on screen %s  (%s 个纯色文字块)" % ("OK" if text_ok else "FAIL", st.get("textBlocks")))
    print("  world layer    %s" % ("OK" if frame_ok else "FAIL"))
    print("  canvas content %s  (dark %s%%, distinct %s)" % (
        "OK" if canvas_ok else "FAIL", st.get("darkPct"), st.get("distinctColors")))
    print("  no exceptions  %s" % ("OK" if not errs else "FAIL"))

    ok = art_ok and glyph_ok and text_ok and frame_ok and canvas_ok and not errs
    print("\n%s" % ("=== LIVE OK: the site works ===" if ok else "=== LIVE FAIL: see above ==="))
    sys.exit(0 if ok else 1)
