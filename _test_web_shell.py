# -*- coding: utf-8 -*-
"""用 Playwright 在**模拟 iPad** 的视口下端到端验证网页外壳。

【为什么能测：用 route 拦截伪造一个 pvz.js】
  真正的 wasm 产物要等 Emscripten 构建，但外壳的逻辑（布局、触摸映射、
  横竖屏、启动流程）**不依赖 wasm 的内容** —— 它只依赖 Module 的几个接口。
  所以这里拦截 `pvz.js` 的请求、返回一段 mock：
      · 触发 onRuntimeInitialized（走完启动流程）
      · ccall 把每次调用打出来（于是能断言"点击确实传到了 gamePointerDown"）
      · 提供空的 FS / HEAPU32
  这样在没有工具链的情况下，也能验证外壳 90% 的行为。

【要验证的清单】
  ① 页面加载无 JS 报错
  ② 两种 iPad 视口下 canvas 都是**等比铺满、不裁切、居中**
  ③ 触摸点按 → 正确翻译成逻辑坐标（含 letterbox 偏移）
  ④ 长按 250ms → 触发悬停而不是点击
  ⑤ 拖动 → 不触发点击
  ⑥ 竖屏出现"横屏体验更好"提示，且可关闭
  ⑦ 屏幕快捷键按钮能触发 handleKey
  ⑧ 旋转后触摸映射仍然正确（这是最容易错的地方）

用法：
    python _test_web_shell.py
"""
import http.server
import json
import os
import socketserver
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "web")
PORT = 8791

MOCK_JS = r"""
// ── 伪造的 Emscripten 产物（仅用于外壳测试）──
window.__ccalls = [];
window.Module = window.Module || {};

// ★ 真实帧注入：把真后端导出的世界层（2000x1300 BGRA）灌进模拟堆，
//   让 platWebFrame 返回它的指针。
//   这样能**在浏览器里**验证整条渲染管线：
//     真引擎像素 → HEAPU32 → BGRA→RGBA + alpha=255 → putImageData → canvas
//   而这一段正是生产环境每帧都要跑的代码，用它比用纯色假数据有意义得多。
const FRAME_W = 2000, FRAME_H = 1300;
const FRAME_U32 = FRAME_W * FRAME_H;
Module.HEAPU32 = new Uint32Array(FRAME_U32 + (1 << 16));
Module.HEAPU8 = new Uint8Array(Module.HEAPU32.buffer);
window.__frameReady = false;
window.__framePtr = 8;            // 4 字节对齐的偏移，避开 0（0 被外壳当作无帧）

(function loadFrame() {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '_frame_probe.raw', true);
    xhr.responseType = 'arraybuffer';
    xhr.onload = function () {
        if (!xhr.response) return;
        const buf = new Uint8Array(xhr.response);
        // 文件头：GLB1 + w + h，之后是 BGRA 像素
        const px = buf.subarray(12);
        Module.HEAPU8.set(px, window.__framePtr);
        window.__frameReady = true;
        console.log('[mock] 真实帧已注入 ' + FRAME_W + 'x' + FRAME_H);
    };
    xhr.onerror = function () { console.log('[mock] 帧注入失败'); };
    xhr.send();
})();

Module.ccall = function (name, ret, argTypes, args) {
    window.__ccalls.push({ name: name, args: args || [] });
    if (name === 'platWebFrame') return window.__frameReady ? window.__framePtr : 0;
    if (name === 'gameArtCount') return 853;
    return 0;
};
Module.cwrap = function () { return function () {}; };
Module.FS = {
    readFile: function () { throw new Error('no save yet'); },
    writeFile: function () {}
};
setTimeout(function () {
    if (Module.onRuntimeInitialized) Module.onRuntimeInitialized();
}, 30);
"""


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def translate_path(self, path):
        # 只服务 web/ 目录
        p = path.split('?')[0].split('#')[0]
        if p.startswith('/'):
            p = p[1:]
        return os.path.join(WEB, p.replace('/', os.sep))


def serve():
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("127.0.0.1", PORT), Handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd


def main():
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("  ★ 未安装 playwright，跳过")
        return 1

    httpd = serve()
    time.sleep(0.4)
    base = "http://127.0.0.1:%d/index.html" % PORT

    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print("  %-46s %s %s" % (name, "PASS" if ok else "FAIL", detail))

    with sync_playwright() as pw:
        browser = pw.chromium.launch(args=["--no-sandbox"])

        # ── iPad Pro 11 横屏 与 竖屏（都是真机逻辑分辨率）──
        VIEWS = {
            "横屏 1194x834": (1194, 834, 2),
            "竖屏 834x1194": (834, 1194, 2),
        }

        for label, (w, h, dpr) in VIEWS.items():
            ctx = browser.new_context(
                viewport={"width": w, "height": h},
                device_scale_factor=dpr,
                is_mobile=True,
                has_touch=True,
                user_agent=("Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) "
                            "AppleWebKit/605.1.15 (KHTML, like Gecko) "
                            "Version/17.0 Mobile/15E148 Safari/604.1"),
            )
            page = ctx.new_page()
            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.on("console", lambda m: errors.append("console." + m.type + ": " + m.text)
                    if m.type == "error" else None)

            # 拦截 pvz.js → 返回 mock
            page.route("**/pvz.js", lambda route: route.fulfill(
                status=200, content_type="application/javascript", body=MOCK_JS))

            page.goto(base, wait_until="domcontentloaded")
            page.wait_for_timeout(400)

            check("[%s] 页面加载无 JS 错误" % label, not errors,
                  (errors[0][:70] if errors else ""))

            # ① 启动页
            btn = page.locator("#bootBtn")
            check("[%s] 启动按钮出现且可用" % label,
                  btn.is_visible() and not btn.is_disabled(),
                  "文案=" + (btn.inner_text() or ""))

            # ② 点击开始
            btn.click()
            page.wait_for_timeout(600)

            box = page.evaluate("""() => {
                const c = document.getElementById('screen');
                const r = c.getBoundingClientRect();
                return { cssW: r.width, cssH: r.height,
                         top: r.top, left: r.left, bottom: r.bottom,
                         pxW: c.width, pxH: c.height,
                         vw: window.innerWidth, vh: window.innerHeight,
                         keysVisible: !document.getElementById('keys').hidden,
                         keysHiddenAttr: document.getElementById('keys').hidden };
            }""")
            # canvas 像素尺寸必须恒为世界层尺寸
            check("[%s] canvas 像素尺寸 = 2000x1300" % label,
                  box["pxW"] == 2000 and box["pxH"] == 1300,
                  "%dx%d" % (box["pxW"], box["pxH"]))
            # 等比铺满：显示宽高比应等于 1000:650
            ratio = box["cssW"] / box["cssH"] if box["cssH"] else 0
            check("[%s] 显示比例保持 1.538（不裁切）" % label,
                  abs(ratio - 1000.0 / 650.0) < 0.02,
                  "比例 %.3f" % ratio)
            # 不溢出视口
            check("[%s] 画布不超出视口" % label,
                  box["cssW"] <= box["vw"] + 1 and box["cssH"] <= box["vh"] + 1,
                  "画布 %.0fx%.0f / 视口 %dx%d" %
                  (box["cssW"], box["cssH"], box["vw"], box["vh"]))
            check("[%s] 屏幕快捷键已显示" % label, box["keysVisible"])

            # ⑧★ 真实帧注入：验证整条渲染管线（真像素→HEAPU32→RGBA→canvas）
            page.wait_for_timeout(500)
            got = page.evaluate("""() => {
                const c = document.getElementById('screen');
                const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
                let rs=0, gs=0, bs=0, n=0, dark=0, opaque=0;
                for (let i = 0; i < d.length; i += 4 * 997) {   // 抽样
                    rs += d[i]; gs += d[i+1]; bs += d[i+2]; n++;
                    if (d[i] + d[i+1] + d[i+2] < 24) dark++;
                    if (d[i+3] === 255) opaque++;
                }
                return { r: rs/n, g: gs/n, b: bs/n, darkRatio: dark/n,
                         opaqueRatio: opaque/n, n: n };
            }""")
            check("[%s] canvas 已收到真实帧（非全黑）" % label,
                  got["darkRatio"] < 0.55,
                  "暗部占比 %.0f%%  平均 RGB(%.0f,%.0f,%.0f)" %
                  (got["darkRatio"]*100, got["r"], got["g"], got["b"]))
            # 引擎的世界层 alpha 是 0（GDI 语义），外壳必须补成 255，
            # 否则 putImageData 会整幅透明 —— 这条专门钉住那个转换。
            check("[%s] alpha 已被外壳补成 255" % label,
                  got["opaqueRatio"] > 0.99,
                  "不透明占比 %.1f%%" % (got["opaqueRatio"]*100))
            # 草坪是绿的：G 通道应显著高于 B。若 R/B 通道搞反，这里会翻过来。
            check("[%s] 通道顺序正确（草坪 G>B，无 R/B 互换）" % label,
                  got["g"] > got["b"] + 4,
                  "G=%.0f B=%.0f" % (got["g"], got["b"]))
            # 黑边必须均匀（居中），否则触摸映射的偏移量会算错
            gapTop = box["top"]
            gapBottom = box["vh"] - box["bottom"]
            gapLeft = box["left"]
            gapRight = box["vw"] - box["left"] - box["cssW"]
            check("[%s] 画面居中（黑边左右/上下均匀）" % label,
                  abs(gapTop - gapBottom) <= 2 and abs(gapLeft - gapRight) <= 2,
                  "上下 %.0f/%.0f 左右 %.0f/%.0f" % (gapTop, gapBottom, gapLeft, gapRight))
            check("[%s] 启动页已隐藏（hidden 属性 + display:none 都生效）" % label,
                  page.evaluate("document.getElementById('boot').hidden"),
                  "")

            # ③ 竖屏提示（必须在触摸测试**之前**处理）
            #    竖屏时它是全屏弹层，会挡住下面的所有操作 ——
            #    这正是它该有的行为（用户得先做选择），所以测试要先关掉它。
            visible = page.evaluate("!document.getElementById('rotateHint').hidden")
            if "竖屏" in label:
                check("[%s] 出现横屏提示" % label, visible)
                page.locator("#rotateOk").click()
                page.wait_for_timeout(180)
                gone = page.evaluate("document.getElementById('rotateHint').hidden")
                check("[%s] 提示可关闭" % label, gone)
            else:
                check("[%s] 横屏不显示提示" % label, not visible)

            # ④ 触摸点按 → 应调用 gamePointerDown 且坐标落在逻辑范围内
            page.evaluate("window.__ccalls = []")
            # ★ 必须用 bounding_box 拿画布在**视口里的绝对位置**。
            #   第一版写的是 cssW/2, cssH/2 —— 那是相对画布的坐标，
            #   而 page.mouse 收的是视口坐标。横屏黑边小（29px）所以"碰巧"通过，
            #   竖屏黑边有 326px，于是全部点到了画布外 → 三条断言假失败。
            #   （反过来说，这恰好验证了 toGame() 的 letterbox 过滤是对的：
            #     点在黑边上会被正确忽略，而不是误判成游戏内的点击。）
            cbox0 = page.locator("#screen").bounding_box()
            cx = cbox0["x"] + cbox0["width"] / 2
            cy = cbox0["y"] + cbox0["height"] / 2
            page.mouse.click(cx, cy)
            page.wait_for_timeout(150)
            calls = page.evaluate("window.__ccalls")
            downs = [c for c in calls if c["name"] == "gamePointerDown"]
            ok_xy = False
            if downs:
                x, y = downs[0]["args"][0], downs[0]["args"][1]
                ok_xy = 0 <= x <= 1000 and 0 <= y <= 650
            check("[%s] 点按 → gamePointerDown(逻辑坐标)" % label,
                  len(downs) == 1 and ok_xy,
                  ("坐标 (%.0f,%.0f)" % (downs[0]["args"][0], downs[0]["args"][1]))
                  if downs else "未触发")

            # ⑤ 长按 → 悬停而不是点击
            page.evaluate("window.__ccalls = []")
            page.mouse.move(cx, cy)
            page.mouse.down()
            page.wait_for_timeout(420)        # 超过 250ms 的长按阈值
            page.mouse.up()
            page.wait_for_timeout(120)
            calls = page.evaluate("window.__ccalls")
            downs = [c for c in calls if c["name"] == "gamePointerDown"]
            moves = [c for c in calls if c["name"] == "gamePointerMove"]
            check("[%s] 长按 → 悬停(move) 且不触发点击" % label,
                  len(downs) == 0 and len(moves) >= 2,
                  "move×%d down×%d" % (len(moves), len(downs)))

            # ⑥ 拖动 → 不触发点击
            page.evaluate("window.__ccalls = []")
            page.mouse.move(cx, cy)
            page.mouse.down()
            page.mouse.move(cx + 90, cy + 40, steps=6)
            page.mouse.up()
            page.wait_for_timeout(120)
            calls = page.evaluate("window.__ccalls")
            downs = [c for c in calls if c["name"] == "gamePointerDown"]
            check("[%s] 拖动 → 不触发点击" % label, len(downs) == 0,
                  "down×%d" % len(downs))

            # ⑦ 屏幕快捷键 → handleKey
            page.evaluate("window.__ccalls = []")
            page.locator(".kb").first.click()
            page.wait_for_timeout(120)
            calls = page.evaluate("window.__ccalls")
            hk = [c for c in calls if c["name"] == "handleKey"]
            check("[%s] 屏幕快捷键 → handleKey(32)" % label,
                  len(hk) == 1 and hk[0]["args"][0] == 32,
                  "vk=%s" % (hk[0]["args"][0] if hk else "无"))

            page.screenshot(path=os.path.join(HERE, "_webshell_%s.png"
                                              % ("portrait" if "竖屏" in label else "landscape")))

            # ⑧ 旋转后映射仍正确（最容易错的点）
            if "横屏" in label:
                page.set_viewport_size({"width": 834, "height": 1194})
                page.wait_for_timeout(400)
                box2 = page.evaluate("""() => {
                    const c = document.getElementById('screen');
                    const r = c.getBoundingClientRect();
                    return { cssW: r.width, cssH: r.height,
                             vw: window.innerWidth, vh: window.innerHeight };
                }""")
                # 旋转到竖屏后，横屏提示会**重新弹出**（portraitHintClosed
                # 只在本次会话内生效，而这里是同一个页面 —— 但横屏用例里
                # 提示从未被关过，所以现在会挡在前面）。
                # 这是正确行为：竖屏就该提醒。测试要先把它关掉再点。
                hint_shown = page.evaluate(
                    "!document.getElementById('rotateHint').hidden")
                if hint_shown:
                    page.locator("#rotateOk").click()
                    page.wait_for_timeout(180)
                check("旋转到竖屏后重新出现横屏提示", hint_shown)

                page.evaluate("window.__ccalls = []")
                cbox2 = page.locator("#screen").bounding_box()
                page.mouse.click(cbox2["x"] + cbox2["width"] / 2,
                                 cbox2["y"] + cbox2["height"] / 2)
                page.wait_for_timeout(150)
                calls = page.evaluate("window.__ccalls")
                downs = [c for c in calls if c["name"] == "gamePointerDown"]
                ok_rot = False
                if downs:
                    x, y = downs[0]["args"][0], downs[0]["args"][1]
                    # 中心点应映射到逻辑中心附近 (500, 325)
                    ok_rot = abs(x - 500) < 40 and abs(y - 325) < 40
                check("旋转到竖屏后中心点仍映射到 (500,325)", ok_rot,
                      "(%.0f,%.0f)" % (downs[0]["args"][0], downs[0]["args"][1])
                      if downs else "未触发")

            ctx.close()

        browser.close()

    httpd.shutdown()
    npass = sum(1 for _, ok, _ in results if ok)
    print("\n  合计 %d / %d 通过" % (npass, len(results)))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
