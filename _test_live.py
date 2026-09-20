# -*- coding: utf-8 -*-
"""线上站点端到端验证：真浏览器加载 poker-pfr.site/pvz/，确认 WASM 能启动、能出画面。

为什么要做这一步（而不是只 curl 文件）：
  curl 只能证明"文件能下载"。而网页版的失败模式大多是**运行期**的：
    · wasm 加载失败 / 实例化报错
    · pvz.data 读不出来（MEMFS 路径不对）
    · 初始化的 JS 报错导致按钮永远不可点
  这些只有真跑一遍浏览器才发现。

⚠️ DNS 绕行：本机 DNS 被沙箱拦截（poker-pfr.site 解析到 198.18.x.x 保留段），
   所以用 Chromium 的 --host-resolver-rules 把域名映射到真实 IP。
   比改 hosts 文件干净（不影响本机其它程序，也不需要管理员权限）。
"""
import sys
from playwright.sync_api import sync_playwright

URL = "https://poker-pfr.site/pvz/"
REAL_IP = "111.229.27.13"

with sync_playwright() as pw:
    browser = pw.chromium.launch(args=[
        "--no-sandbox",
        "--host-resolver-rules=MAP poker-pfr.site %s,MAP www.poker-pfr.site %s" % (REAL_IP, REAL_IP),
        "--ignore-certificate-errors",
    ])
    ctx = browser.new_context(
        viewport={"width": 1194, "height": 834},   # iPad Pro 11 横屏
        device_scale_factor=2,
        is_mobile=True, has_touch=True,
    )
    page = ctx.new_page()

    logs, errors, failed = [], [], []
    page.on("console", lambda m: logs.append("[%s] %s" % (m.type, m.text)))
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.on("requestfailed", lambda r: failed.append("%s :: %s" % (r.url, r.failure)))

    print("① 打开 %s（DNS 映射到 %s）" % (URL, REAL_IP))
    page.goto(URL, wait_until="domcontentloaded", timeout=90000)
    page.wait_for_timeout(3000)
    print("   标题: %s" % page.title())

    print("② 等引导页就绪（要下 76MB 的 pvz.data）…")
    ok = False
    for i in range(60):          # 最多等 5 分钟
        txt = page.inner_text("#bootBtn")
        disabled = page.eval_on_selector("#bootBtn", "el => el.disabled")
        msg = page.inner_text("#bootMsg")
        if i % 6 == 0:
            print("   [%3ds] 按钮='%s' disabled=%s  状态='%s'" % (i*5, txt.strip(), disabled, msg.strip()[:60]))
        if not disabled:
            ok = True
            break
        page.wait_for_timeout(5000)

    if not ok:
        print("   ★ 引导页始终不可点 —— 说明运行时初始化失败")
        for l in logs[-25:]: print("     ", l)
        for e in errors[-8:]: print("     ERR:", e)
        for f in failed[:8]: print("     FAILED:", f)
        page.screenshot(path="_live_fail.png", full_page=True)
        browser.close(); sys.exit(1)

    print("③ 点击开始")
    page.click("#bootBtn")
    page.wait_for_timeout(4000)

    print("④ 检查画面是否有内容")
    got = page.evaluate("""() => {
        const c = document.getElementById('screen');
        const d = c.getContext('2d').getImageData(0,0,c.width,c.height).data;
        let rs=0,gs=0,bs=0,n=0,dark=0;
        for (let i=0;i<d.length;i+=4*997){rs+=d[i];gs+=d[i+1];bs+=d[i+2];n++;
            if(d[i]+d[i+1]+d[i+2]<24)dark++;}
        return {r:rs/n,g:gs/n,b:bs/n,dark:dark/n,w:c.width,h:c.height};
    }""")
    print("   canvas %dx%d  平均 RGB(%.0f,%.0f,%.0f)  暗部 %.0f%%"
          % (got["w"], got["h"], got["r"], got["g"], got["b"], got["dark"]*100))

    page.screenshot(path="_live_ipad.png")
    page.wait_for_timeout(1500)
    page.screenshot(path="_live_ipad2.png")

    print("⑤ 控制台里与资源/错误相关的行")
    for l in logs:
        if any(k in l.lower() for k in ("error", "fail", "404", "undefined", "not a function", "abort")):
            print("   ", l[:160])

    print("⑥ JS 异常: %d 条" % len(errors))
    for e in errors[:5]: print("   ", e[:200])

    print("⑦ 失败请求: %d 条" % len(failed))
    for f in failed[:8]: print("   ", f[:180])

    browser.close()

    legit = got["dark"] < 0.6 and got["g"] > got["b"] - 20
    print("\n%s" % ("✓ 线上站点可用（画面有内容、无致命 JS 异常）" if legit and not errors else "★ 有问题，见上面"))
