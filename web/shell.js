/* =========================================================================
   shell.js —— 网页版外壳（面向 iPad）
   -------------------------------------------------------------------------
   这一层只做四件事，其余全交给 wasm 里的游戏：
     ① 驱动帧循环（rAF → gameStep → 取世界层 → 上传 canvas）
     ② 把触摸事件翻译成**逻辑游戏坐标**（含 letterbox 与长按→悬停）
     ③ 音频（音效走 Web Audio，BGM 走 HTMLAudio 流式）
     ④ 存档持久化（把 MEMFS 里的 pvz_save.dat 同步到 localStorage）

   ★ 为什么输入要"翻译"而不是自己实现命中判定：
     引擎已经有一份完整的命中逻辑（onClick / uiPointerHot / 三选一的
     悬停判定）。外壳只负责把屏幕坐标换算成 1000×650 的逻辑坐标，
     然后把事件丢给 gamePointerDown/Move/Up。
     自己再写一份判定 = 迟早和桌面版分叉（这个项目已经因为"手抄副本"
     翻过三次车）。
   ========================================================================= */
'use strict';

(function () {

/* ───────────────────────── 常量 ───────────────────────── */
const VIEW_W = 1000;          // 引擎逻辑宽（与 pvz.c 的 VIEW_W 一致）
const VIEW_H = 650;           // 引擎逻辑高
const SS     = 2;             // 超采样倍数
const DEV_W  = VIEW_W * SS;   // 世界层像素宽 = 2000
const DEV_H  = VIEW_H * SS;   // 世界层像素高 = 1300

const LONGPRESS_MS = 250;     // 长按多久算"悬停"（触摸没有 hover，必须给替代）
const MOVE_TOL_PX  = 12;      // 按下后移动超过这个距离就不算长按（视作拖动）

/* ───────────────────────── DOM ───────────────────────── */
const $ = (id) => document.getElementById(id);
const bootEl   = $('boot');
const bootMsg  = $('bootMsg');
const bootBar  = $('bootProgBar');
const bootProg = $('bootProgWrap');
const bootBtn  = $('bootBtn');
const bootHint = $('bootHint');
const stageEl  = $('stage');
const canvas   = $('screen');
const ctx2d    = canvas.getContext('2d', { alpha: false, desynchronized: true });
const keysEl   = $('keys');
const rotateEl = $('rotateHint');
const diagEl   = $('diag');

const DEBUG = /[?&]debug=1/.test(location.search);

/* ───────────────────────── 运行时状态 ───────────────────────── */
let Module = null;
let imgData = null;                 // 复用的 ImageData（避免每帧分配 10MB）
let imgU32  = null;                 // 上面那张的 Uint32 视图
let running = false;
let lastT   = 0;
let viewScale = 1, viewOffX = 0, viewOffY = 0;   // canvas 在舞台里的位置与缩放
let frameCount = 0, uploadMsSum = 0, uploadMsN = 0, lastDiagT = 0;

/* 触摸状态：同时只跟一根手指（游戏是单点操作，多点只会误触） */
const touch = {
    id: null,
    downX: 0, downY: 0,        // 逻辑坐标
    downT: 0,
    moved: false,
    longTimer: 0,
    hoverSent: false,
    active: false
};

/* ───────────────────────── 诊断 ───────────────────────── */
function log(msg) {
    if (DEBUG) console.log('[pvz]', msg);
    const line = String(msg);
    if (DEBUG && diagEl) {
        diagEl.textContent = (diagEl.textContent + '\n' + line).split('\n').slice(-12).join('\n');
    }
}

/* ★ 错误一律打出去，**不受 DEBUG 开关控制**。
   教训（2026-09-15）：字形注入里 catch 到异常后调的是 log()，
   而 log() 只在 ?debug=1 时才 console.log —— 于是异常被完全吞掉，
   表现成"字形只灌进去一部分、画面上是一排占位方块"，
   而我在这边看控制台是干净的、以为没有报错。
   一个只在调试模式下才说话的报错，等于没有报错。 */
function logErr(msg) {
    try { console.error('[pvz][ERR]', msg); } catch (_) {}
    if (diagEl) {
        diagEl.hidden = false;
        diagEl.textContent = (diagEl.textContent + '\n[ERR] ' + String(msg))
            .split('\n').slice(-12).join('\n');
    }
}

/* ═════════════════════════ ① 布局与坐标映射 ═════════════════════════
   游戏是 1000×650（≈3:2），iPad 有 4:3 / 1.43，比例都对不上。
   策略：**等比铺满、居中留边**（contain，不是 cover）。
   cover 会把草坪两侧切掉 —— 而草坪边缘正是"僵尸从哪来"的信息。
   ─────────────────────────────────────────────────────────────────── */
function layout() {
    const padT = 0, padL = 0;      // 安全区已经在 CSS 的 padding 里
    const availW = stageEl.clientWidth;
    const availH = stageEl.clientHeight;
    if (availW <= 0 || availH <= 0) return;

    /* 用世界层像素尺寸算缩放，再换成 CSS 尺寸 —— 等价，但读数更直观 */
    const s = Math.min(availW / VIEW_W, availH / VIEW_H);
    const cssW = Math.round(VIEW_W * s);
    const cssH = Math.round(VIEW_H * s);

    canvas.style.width  = cssW + 'px';
    canvas.style.height = cssH + 'px';

    /* 记录映射参数：屏幕(舞台内)坐标 → 逻辑坐标 */
    const rect = canvas.getBoundingClientRect();
    const stageRect = stageEl.getBoundingClientRect();
    viewScale = s;
    viewOffX = rect.left - stageRect.left + padL;
    viewOffY = rect.top  - stageRect.top  + padT;
    log('layout ' + availW + 'x' + availH + ' -> css ' + cssW + 'x' + cssH +
        ' scale=' + s.toFixed(3));
}

/* 事件坐标 → 逻辑游戏坐标。返回 null 表示落在黑边里（不算点中）。 */
function toGame(clientX, clientY) {
    const stageRect = stageEl.getBoundingClientRect();
    const x = clientX - stageRect.left - viewOffX;
    const y = clientY - stageRect.top  - viewOffY;
    const gx = x / viewScale;
    const gy = y / viewScale;
    if (gx < -8 || gy < -8 || gx > VIEW_W + 8 || gy > VIEW_H + 8) return null;
    return {
        x: Math.max(0, Math.min(VIEW_W - 1, gx)),
        y: Math.max(0, Math.min(VIEW_H - 1, gy))
    };
}

/* ═════════════════════════ ② 触摸 → 游戏输入 ═════════════════════════
   桌面端的三种操作在触摸上的映射：
     鼠标移动 → 手指拖动（种植时卡片跟随）
     左键点击 → 点按
     **悬停**（预览/高亮）→ 长按 250ms      ← 触摸没有 hover，必须给替代
     右键（铲子）      → 屏幕上的铲子按钮（由游戏内 UI 提供）
   ─────────────────────────────────────────────────────────────────── */
function cancelLongPress() {
    if (touch.longTimer) { clearTimeout(touch.longTimer); touch.longTimer = 0; }
}

function onDown(clientX, clientY, id) {
    if (!running) return false;
    const p = toGame(clientX, clientY);
    if (!p) return false;                    // 点在黑边上，忽略

    touch.id = id;
    touch.active = true;
    touch.downX = p.x; touch.downY = p.y;
    touch.downT = performance.now();
    touch.moved = false;
    touch.hoverSent = false;

    /* 先按"移动"上报：让卡片/按钮的悬停高亮立刻亮起来。
       引擎的 gDraftHover 只由 move 更新，所以拖拽种植必须先有一帧 move。 */
    Module.ccall('gamePointerMove', null, ['float', 'float'], [p.x, p.y]);

    /* 长按 → 补一次悬停语义（触摸没有 hover） */
    cancelLongPress();
    touch.longTimer = setTimeout(function () {
        touch.longTimer = 0;
        if (touch.moved || !touch.active) return;
        touch.hoverSent = true;
        /* 悬停在引擎里就是"指针停在这里不动"——再发一次 move 即可，
           引擎的 gDraftHover / uiPointerHot 会据此更新。 */
        Module.ccall('gamePointerMove', null, ['float', 'float'], [touch.downX, touch.downY]);
        log('longpress hover @' + touch.downX.toFixed(0) + ',' + touch.downY.toFixed(0));
    }, LONGPRESS_MS);

    /* ⚠️ 点按的"按下"**不立刻**交给游戏。
       原因：引擎的 onClick 是在按下时触发的（不是抬起），
       如果这里立刻转发，长按就永远没机会先触发 —— 会先种下去。
       所以按下只更新悬停，真正的点击在**抬起**时按"位移是否超阈值"决定。 */
    return true;
}

function onMove(clientX, clientY) {
    if (!running) return;
    const p = toGame(clientX, clientY);
    if (!p) return;
    if (touch.active) {
        const dx = p.x - touch.downX, dy = p.y - touch.downY;
        if (dx * dx + dy * dy > MOVE_TOL_PX * MOVE_TOL_PX) {
            touch.moved = true;
            cancelLongPress();               // 拖动起来了，就不算长按
        }
    }
    Module.ccall('gamePointerMove', null, ['float', 'float'], [p.x, p.y]);
}

function onUp(clientX, clientY) {
    if (!running) return;
    cancelLongPress();
    const p = toGame(clientX, clientY) || { x: touch.downX, y: touch.downY };
    const wasActive = touch.active;
    touch.active = false;
    touch.id = null;
    if (!wasActive) return;

    /* 抬起时判定"算不算点击"：
         · 位移超过阈值 → 这是拖拽（种植手势），**不**当成点击；
           引擎在拖拽种植里用的是"移动时跟随、松开时落下"，
           pvz.c 的 onClick 只处理点击，所以这里不转发移动端没有的语义。
         · 长按已经触发过 → 也不算点击（用户在看信息）
         · 其余 → 转发为点击 */
    if (touch.moved) { log('drag end, no click'); return; }
    if (touch.hoverSent) { log('longpress, no click'); return; }
    Module.ccall('gamePointerDown', null, ['float', 'float'], [p.x, p.y]);
    Module.ccall('gamePointerUp', null, []);
}

/* ---- 指针事件绑定 ----
   ⚠️ 用 Pointer Events 而不是 Touch Events：
      iPadOS 13+ 的 Safari 完整支持 Pointer Events，而且它**自动统一**
      了鼠标 / 触摸 / 触控笔 —— 在桌面上用鼠标调试时不用改代码。
      （用 Touch Events 的话，桌面 Chrome 的触摸模拟与真实 iPad 行为
        会有差异，容易"本地好好的、实机不对"。） */
function bindPointer() {
    const opt = { passive: false };
    canvas.addEventListener('pointerdown', function (e) {
        if (touch.active) return;                 // 已经在跟一根手指，忽略第二根
        if (onDown(e.clientX, e.clientY, e.pointerId)) {
            e.preventDefault();
            try { canvas.setPointerCapture(e.pointerId); } catch (_) {}
        }
    }, opt);

    canvas.addEventListener('pointermove', function (e) {
        if (touch.active && e.pointerId !== touch.id) return;
        onMove(e.clientX, e.clientY);
        e.preventDefault();
    }, opt);

    const up = function (e) {
        if (touch.active && e.pointerId !== touch.id) return;
        onUp(e.clientX, e.clientY);
        e.preventDefault();
    };
    canvas.addEventListener('pointerup', up, opt);
    canvas.addEventListener('pointercancel', function (e) {
        cancelLongPress();
        touch.active = false; touch.id = null;
        Module && Module.ccall('gamePointerUp', null, []);
        e.preventDefault();
    }, opt);

    /* 双保险：iOS 上 viewport 的 user-scalable=no 不总生效 */
    document.addEventListener('gesturestart', function (e) { e.preventDefault(); });
    document.addEventListener('gesturechange', function (e) { e.preventDefault(); });
    document.addEventListener('dblclick', function (e) { e.preventDefault(); });
    /* 长按弹出的系统菜单 / 文本选择 */
    document.addEventListener('contextmenu', function (e) { e.preventDefault(); });
}

/* ═════════════════════════ ③ 帧循环 ═════════════════════════
   一帧的动作：
     gameStep(dt, out)         → 引擎推进 + 渲染到世界层
     platWebFrame()            → 拿世界层指针与尺寸
     BGRA → RGBA + alpha=255   → 写进 ImageData
     putImageData              → 上屏

   ★ 为什么 alpha 要自己填 255：
     引擎的软件光栅**故意不写 alpha**（GDI 的填充图元就是只写 RGB，
     实测确认）。这是为了让"世界层字节流"与 Windows 版逐字节可比 ——
     比对基线会 dump 全部 4 字节，写 255 就永远对不上。
     canvas 的 putImageData 则**必须**有 alpha，否则整幅画面全透明。
     所以这个转换放在外壳做，不放在光栅后端做。
   ─────────────────────────────────────────────────────────── */
/* ═════════════════════════ 字形光栅化（文字） ═════════════════════════
   为什么需要它：引擎的软件光栅没有字体引擎，文字必须由 JS 用 Canvas2D
   光栅化成位图、灌回 wasm 的字形表。没有这一步的后果是**整屏一个字的都没有** ——
   阳光数、卡价、波次、悬停提示、菜单全部消失，而画面其余部分完全正常，
   看起来像"UI 没做"，其实是"字没送进去"。

   契约（与 plat_web.c 严格对齐，改一边必须改另一边）：
     · px 是**设备**像素（= 逻辑字号 × 世界层缩放）。世界层是 2x，
       所以逻辑 15px 的字要按 30px 光栅化；按 15px 做会被放大成糊的。
     · 位图的**基线在第 px 行**（从顶往下数）。引擎侧 curY 就是基线，
       贴图起点 = curY - px。
     · 位图只存**覆盖率**（白色 + alpha）。颜色由引擎的 SetTextColor 现算，
       所以这里**绝不能**染成别的颜色，否则所有文字都会是同一个色。
     · 光栅化时若某字的实际 ascent 超过 px，宁可略微缩小字号，
       也不能切顶 —— 切顶的字比小字难看得多。

   为什么用「引擎来取」而不是「外壳预先铺一遍字符表」：
     引擎的文案大量是运行时拼的（阳光 123、第 4/10 波、植物名…），
     外壳硬编码必然漏字。所以由引擎在查不到字形时登记请求，外壳来取。
   ─────────────────────────────────────────────────────────────────── */
const GLYPH_FONT = '"Microsoft YaHei", "PingFang SC", "Hiragino Sans GB", "Heiti SC", "WenQuanYi Micro Hei", sans-serif';
let glyphCount = 0;
let glyphFails = 0;             /* 注入被拒/参数非法 的次数，供自检用 */

/* 每次 pump 之后调用：若"有请求待处理、但一个都没灌进去"连续多帧，
   就吼一声。2026-09-15 那个 const 遮蔽 bug 就是靠这个才能被立刻发现 ——
   它不会报错，只会让字形表永远填不满、画面上大半是占位色块。 */
let glyphStall = 0;
function glyphWatchdog(made, pending) {
    if (pending && made === 0) {
        if (++glyphStall === 12) {
            logErr('字形注入连续 12 帧零进展（待处理请求仍在）—— 字形管线卡住了');
        }
    } else {
        glyphStall = 0;
    }
}

/* 单个字形的离屏画布复用（每次 new 一个 canvas 太慢，2000 个字会明显卡） */
const glyphCv = document.createElement('canvas');
const glyphCtx = glyphCv.getContext('2d', { willReadFrequently: true });

/* 返回 { w, h, adv, cov } —— cov 是覆盖率数组（每像素 1 字节） */
function rasterGlyph(cp, px, bold) {
    const ch = String.fromCodePoint(cp);
    const base = (bold ? 'bold ' : '') + px + 'px ' + GLYPH_FONT;

    /* ① 先量 ascent，必要时缩字号，避免切顶 */
    glyphCtx.font = base;
    let m = glyphCtx.measureText(ch);
    let asc = Math.ceil(m.actualBoundingBoxAscent || px * 0.9);
    let size = px;
    if (asc > px - 1 && asc > 0) {
        size = Math.max(1, Math.floor(px * (px - 1) / asc));
        glyphCtx.font = (bold ? 'bold ' : '') + size + 'px ' + GLYPH_FONT;
        m = glyphCtx.measureText(ch);
        asc = Math.ceil(m.actualBoundingBoxAscent || size * 0.9);
    }

    const adv  = Math.max(1, Math.round(m.width));
    const desc = Math.max(2, Math.ceil((m.actualBoundingBoxDescent || size * 0.25)));
    const w = Math.max(1, Math.ceil(m.width) + 2);
    const h = Math.min(px + desc + 2, Math.max(asc + 2, px + 2));

    glyphCv.width = w;
    glyphCv.height = h;
    /* 改尺寸会重置上下文状态，所以要重新设 font */
    glyphCtx.font = (bold ? 'bold ' : '') + size + 'px ' + GLYPH_FONT;
    glyphCtx.textBaseline = 'alphabetic';
    glyphCtx.textAlign = 'left';
    glyphCtx.clearRect(0, 0, w, h);
    glyphCtx.fillStyle = '#fff';              /* 只画覆盖率，颜色交给引擎 */
    glyphCtx.fillText(ch, 1, px);             /* ★ 基线落在第 px 行 */

    const d = glyphCtx.getImageData(0, 0, w, h).data;
    const cov = new Uint8Array(w * h);
    for (let i = 0, j = 3; i < cov.length; i++, j += 4) cov[i] = d[j];
    return { w, h, adv, cov };
}

/* 从引擎取请求 → 光栅化 → 回灌。limit 控制单帧最多做几个（别让首帧卡住）。
   返回本帧实际注入的字形数。 */
function pumpGlyphs(limit) {
    if (!Module || !Module.ccall) return 0;
    /* ⚠️ 变量命名：这个函数里**绝不能**再出现单个字母 n。
       2026-09-15 这里写成 `let n = 0` 当计数器、循环体里又写了
       `const n = g.cov.length`，于是 `n++` 变成"给 const 赋值" →
       TypeError → 被下面的 catch 吞掉 → **每帧只注入 1 个字形**。
       症状：字形表永远填不满（只进去一小部分），画面上大部分文字是
       C 侧的占位色块，看起来像"字体没接上"。所以这里用 done / total 这类
       不会撞车的名字。 */
    let done = 0;
    const p = Module._malloc(12);            /* 3 个 int32 */
    try {
        while (done < limit) {
            const ok = Module.ccall('platWebGlyphReq', 'number',
                                    ['number', 'number', 'number'], [p, p + 4, p + 8]);
            if (!ok) break;
            const cp   = Module.HEAP32[p >> 2];
            const px   = Module.HEAP32[(p >> 2) + 1];
            const bold = Module.HEAP32[(p >> 2) + 2];
            if (px <= 0 || px > 512) { glyphFails++; continue; }   /* 异常字号：丢弃并记一笔 */

            const g = rasterGlyph(cp, px, bold);

            /* ★★ 必须给满 w*h*4 字节（预乘 BGRA），不能只给 w*h 的覆盖率。
               platWebGlyph 内部是 `memcpy(dst, src, gw*gh*4)` —— 只传 w*h
               就是**越界读**：它会去啃堆里紧邻的别人的数据。
               症状非常误导：字形确实"注入成功"、计数也对、画面上也真的有字，
               但字是**颗粒状噪声/条纹**（读到了垃圾），不报任何错。
               本机测试用的是手工构造的 4 通道 buffer，所以本机过、线上坏 ——
               又是"两边不一致"这一类。引擎只读 alpha（覆盖率），字节数必须给足。 */
            const bytes = g.w * g.h * 4;
            const tmp = new Uint32Array(g.w * g.h);
            for (let i = 0; i < tmp.length; i++) {
                const c = g.cov[i];
                tmp[i] = (c << 24) | (c << 16) | (c << 8) | c;   /* 四通道同值（预乘白） */
            }
            const buf = Module._malloc(bytes);
            Module.HEAPU8.set(new Uint8Array(tmp.buffer), buf);
            /* platWebGlyph 内部会 memcpy，所以这里用完即可释放 */
            const rc = Module.ccall('platWebGlyph', 'number',
                         ['number', 'number', 'number', 'number', 'number', 'number', 'number'],
                         [cp, px, bold, buf, g.w, g.h, g.adv]);
            Module._free(buf);
            if (!rc) { glyphFails++; continue; }   /* 表满：记一笔，别静默 */
            glyphCount++;
            done++;
        }
    } catch (e) {
        /* 用 logErr 而不是 log：log 只在 ?debug=1 时才输出，
           而这个异常一旦发生，"字形只灌进去一部分"会被彻底静默掉。 */
        logErr('字形注入异常: ' + (e && e.message ? e.message : e));
    } finally {
        Module._free(p);
    }
    return done;
}

function frame(now) {
    if (!running) return;
    requestAnimationFrame(frame);

    let dt = (now - lastT) / 1000;
    lastT = now;
    if (!isFinite(dt) || dt <= 0) dt = 1 / 60;
    /* 与桌面端同一套钳制（见 WinMain）：切后台回来时 dt 会很大，
       不钳会让僵尸瞬移一大段。 */
    if (dt > 0.05) dt = 0.05;
    if (dt < 0.0005) dt = 0.0005;

    const t0 = performance.now();

    /* ★ 每帧先补字形，再渲染 —— 顺序不能反。
       引擎在排版阶段（GetTextExtentPoint32W）就会登记"缺哪些字"，
       所以先 pump 的话，本帧画的文字就已经有真实字形了。
       早期限流 24 个/帧：首次进入需要约 2000 个字形，
       全部塞进一帧会明显卡一下；分摊到 ~80 帧（1.3 秒）里几乎无感。 */
    {
        const made = pumpGlyphs(glyphCount < 4000 ? 48 : 4);
        /* 还有没有待处理的请求？用来判断"是惰性补字"还是"卡住了" */
        /* ⚠️ 不能用 platWebGlyphReq 探还有没有请求 —— 它是**出队**语义，
           探一次就把那条请求吞掉，那个字永远拿不到字形。
           用不消费的 platWebGlyphReqCount。 */
        let pending = 0;
        try { pending = Module.ccall('platWebGlyphReqCount', 'number', []); } catch (_) {}
        glyphWatchdog(made, pending > 0);
    }

    /* 渲染：out 传一块"假的"屏幕 DC。网页版不读它，
       只靠 platWebFrame() 拿世界层（2x，画质来源）。
       传 gWorldDC 会让世界层被自己的 1x 缩略图覆盖 —— 这个坑
       _test_cards50_visual.c 踩过，画面会出现两套尺度。 */
    Module.ccall('gameStep', null, ['float', 'number'], [dt, 0]);

    const ptr = Module.ccall('platWebFrame', 'number', [], []);
    if (ptr && imgData) {
        /* 世界层在 wasm 堆里。用 HEAPU32 视图做 32 位批量转换 ——
           比逐字节快 3~4 倍（260 万像素 × 60fps，这里必须是快的）。 */
        const n = DEV_W * DEV_H;
        const src = Module.HEAPU32.subarray(ptr >> 2, (ptr >> 2) + n);
        const dst = imgU32;
        for (let i = 0; i < n; i++) {
            const p = src[i];
            /* BGRA(A=0) → RGBA(A=255)
               位运算一次搞定：交换 R/B，并把 alpha 置 255 */
            dst[i] = ((p & 0x000000FF) << 16) |   /* B → R 位 */
                     (p & 0x0000FF00) |           /* G 原位   */
                     ((p & 0x00FF0000) >>> 16) |   /* R → B 位 */
                     0xFF000000;                   /* A = 255  */
        }
        ctx2d.putImageData(imgData, 0, 0);
    }

    const used = performance.now() - t0;
    frameCount++;
    if (frameCount > 8) { uploadMsSum += used; uploadMsN++; }

    if (DEBUG && now - lastDiagT > 500) {
        lastDiagT = now;
        const avg = uploadMsN ? (uploadMsSum / uploadMsN) : 0;
        diagEl.textContent =
            'fps≈' + (1 / dt).toFixed(0) +
            '\n上传+渲染 ' + avg.toFixed(1) + 'ms' +
            '\n画布 ' + canvas.width + 'x' + canvas.height +
            ' 显示 ' + canvas.style.width + 'x' + canvas.style.height +
            '\n设备像素比 ' + (window.devicePixelRatio || 1).toFixed(2);
    }
}

/* ═════════════════════════ ④ 音频 ═════════════════════════
   引擎通过 mciSendStringW / PlaySound 调进来（见 plat_web.c 的桥接）。
   分工：
     · 音效（sfx/*.wav，约 1.3MB）→ 走 MEMFS + Web Audio，低延迟
     · BGM（bgm/*.mp3，约 20MB）→ 走 HTMLAudio 按 URL 流式，不占 wasm 内存

   ★ iOS 的硬要求：AudioContext 必须在**用户手势**里创建/恢复，
     否则全程静音。所以启动按钮（#bootBtn）同时承担解锁职责。
   ─────────────────────────────────────────────────────────── */
const audio = {
    ac: null,            // AudioContext
    unlocked: false,
    sfxBuf: new Map(),   // path → AudioBuffer（解码一次反复用）
    sfxPending: new Map(),
    bgmEl: null,
    bgmPath: '',
    bgmVol: 1000,
    masterGain: null,
    sfxGain: null
};

function unlockAudio() {
    if (audio.unlocked) return true;
    try {
        const AC = window.AudioContext || window.webkitAudioContext;
        if (!AC) { log('AudioContext 不可用，静音运行'); audio.unlocked = true; return false; }
        audio.ac = new AC();
        audio.masterGain = audio.ac.createGain();
        audio.masterGain.gain.value = 1.0;
        audio.masterGain.connect(audio.ac.destination);
        audio.sfxGain = audio.ac.createGain();
        /* 音效整体音量：与 MCI 的 0~1000 量纲换算，见 sfxVolume() */
        audio.sfxGain.gain.value = 1.0;
        audio.sfxGain.connect(audio.masterGain);
        audio.unlocked = true;
        log('AudioContext 已创建，状态=' + audio.ac.state);
        return true;
    } catch (e) {
        log('音频解锁失败: ' + e);
        audio.unlocked = true;
        return false;
    }
}

/* MCI 音量是 0..1000；这里换算成 0..1。
   ⚠️ 按平方衰减（不是线性）：人耳对响度的感知接近对数，
      线性映射会让"音量 110"听起来偏大 —— 桌面端早就把 BGM 压到 11%
      就是这个原因（用户反馈过"背景音乐太大"）。 */
function vol01(mci) {
    const v = Math.max(0, Math.min(1000, mci | 0)) / 1000;
    return v * v;
}

function sfxVolume() { return 0.85; }   /* 引擎给的 SFX_VOLUME 已由 vol01 处理 */

window.PVZ = {
    log: log,

    /* 音效：path 形如 "sfx/hit.wav" */
    sfx: function (path, volMci) {
        if (!audio.ac || audio.ac.state === 'suspended') return;
        const v = vol01(volMci) * sfxVolume();
        if (v <= 0.001) return;

        const play = (buf) => {
            const src = audio.ac.createBufferSource();
            src.buffer = buf;
            const g = audio.ac.createGain();
            g.gain.value = v;
            src.connect(g); g.connect(audio.sfxGain);
            src.start(0);
        };

        const cached = audio.sfxBuf.get(path);
        if (cached) { play(cached); return; }
        if (audio.sfxPending.get(path)) return;      // 正在解码，本次丢弃（避免重复请求）

        /* URL 拼接：与 Emscripten 的 --preload-file 挂载点一致（assets@/assets） */
        const url = 'assets/' + path;
        audio.sfxPending.set(path, true);
        fetch(url)
            .then(r => r.ok ? r.arrayBuffer() : Promise.reject(r.status))
            .then(ab => audio.ac.decodeAudioData(ab))
            .then(buf => {
                audio.sfxBuf.set(path, buf);
                audio.sfxPending.delete(path);
                play(buf);
            })
            .catch(err => {
                audio.sfxPending.delete(path);
                log('音效载入失败 ' + path + ' : ' + err);
            });
    },

    /* BGM：path 形如 "bgm/lv1.mp3" —— 走 HTMLAudio 流式播放。
       为什么不塞进 MEMFS：20MB 常驻内存不划算，而且 HTMLAudio
       支持"边下边播"，进游戏不用等整首下完。 */
    bgmPlay: function (path, volMci, loop) {
        const url = 'assets/' + path;
        if (!audio.bgmEl) {
            audio.bgmEl = new Audio();
            audio.bgmEl.preload = 'auto';
            audio.bgmEl.crossOrigin = 'anonymous';
        }
        if (audio.bgmPath !== path) {
            audio.bgmEl.src = url;
            audio.bgmPath = path;
        }
        audio.bgmVol = volMci;
        /* 同桌面端：BGM 音量被刻意压低（MCI 110/1000 ≈ 11%），
           否则会盖住打击音效。这里用同一套平方衰减。 */
        audio.bgmEl.volume = Math.min(1, vol01(volMci) * 4.0);
        audio.bgmEl.loop = !!loop;
        const pr = audio.bgmEl.play();
        if (pr && pr.catch) pr.catch(err => log('BGM 播放被拒: ' + err));
        return 1;
    },

    bgmStop: function (path) {
        if (audio.bgmEl && (!path || audio.bgmPath === path)) {
            audio.bgmEl.pause();
        }
    },

    bgmPlaying: function (path) {
        if (!audio.bgmEl || (path && audio.bgmPath !== path)) return 0;
        return (!audio.bgmEl.paused && !audio.bgmEl.ended) ? 1 : 0;
    },

    /* 字形自检：已注入的字形数。0 = 没有任何文字会被画出来。
       暴露出来是为了让端到端测试能**断言**这件事 ——
       "整屏无文字"不会抛异常、不会请求失败，只能靠这个数字发现。 */
    glyphCount: function () { return glyphCount; },
    glyphFails: function () { return glyphFails; },
    pumpGlyphs: pumpGlyphs
};

/* ═════════════════════════ ⑤ 存档持久化 ═════════════════════════
   引擎用 _wfopen("pvz_save.dat") 读写 —— 在 wasm 里就是 MEMFS 的一个文件，
   **页面刷新即丢**。所以要在外壳侧把它同步到 localStorage。

   ⚠️ 时机很重要，漏一个就会丢进度：
     · 定时同步（每 5 秒）—— 兜底
     · visibilitychange → 隐藏时立刻同步
       （iOS 会随时冻结后台页面，这是最后的机会）
     · pagehide —— 比 unload 更可靠（iOS 上 unload 常常不触发）
   ─────────────────────────────────────────────────────────── */
const SAVE_KEY = 'pvz_save_v1';
let saveDirty = false;

function saveToStorage() {
    if (!Module || !Module.FS) return;
    try {
        const data = Module.FS.readFile('/pvz_save.dat');
        /* localStorage 只能存字符串。存档是二进制（1252 字节），
           用 latin1 逐字符映射最省（1 字节 → 1 字符），
           不用 base64（会膨胀 33%）。 */
        let s = '';
        const chunk = 0x8000;
        for (let i = 0; i < data.length; i += chunk) {
            s += String.fromCharCode.apply(null, data.subarray(i, i + chunk));
        }
        localStorage.setItem(SAVE_KEY, s);
        saveDirty = false;
    } catch (e) {
        /* 文件还不存在（首次运行、还没触发过存档）—— 正常，不是错误 */
    }
}

function loadFromStorage() {
    if (!Module || !Module.FS) return;
    try {
        const s = localStorage.getItem(SAVE_KEY);
        if (!s) return;
        const data = new Uint8Array(s.length);
        for (let i = 0; i < s.length; i++) data[i] = s.charCodeAt(i) & 0xFF;
        Module.FS.writeFile('/pvz_save.dat', data);
        log('存档已从 localStorage 恢复（' + data.length + ' 字节）');
    } catch (e) {
        log('存档恢复失败: ' + e);
    }
}

function bindPersistence() {
    setInterval(function () { if (running) saveToStorage(); }, 5000);
    document.addEventListener('visibilitychange', function () {
        if (document.hidden) {
            saveToStorage();
            /* 切后台时暂停游戏：iOS 会冻结页面，回来时 dt 会很大。
               置 running=false 让帧循环停下，回来再恢复。 */
            if (running) { running = false; log('切后台，暂停'); }
        } else {
            if (!running && Module) {
                running = true;
                lastT = performance.now();
                requestAnimationFrame(frame);
                log('回到前台，恢复');
            }
        }
    });
    window.addEventListener('pagehide', saveToStorage);
}

/* ═════════════════════════ ⑥ 屏幕快捷键 ═════════════════════════
   iPad 没键盘，把桌面端最常用的几个键做成按钮。
   走的**是同一个** handleKey()（引擎已把它从 WndProc 抽出来变成非 static），
   所以桌面按键与屏幕按钮的行为不可能分叉。
   ─────────────────────────────────────────────────────────── */
function bindKeys() {
    keysEl.querySelectorAll('.kb').forEach(function (btn) {
        const vk = parseInt(btn.dataset.key, 10);
        const fire = function (e) {
            e.preventDefault();
            if (!Module) return;
            /* isRepeat=0：按钮按一次算一次，不模拟长按自动重复 */
            Module.ccall('handleKey', null, ['number', 'number'], [vk, 0]);
            btn.style.filter = 'brightness(1.5)';
            setTimeout(function () { btn.style.filter = ''; }, 90);
        };
        btn.addEventListener('pointerdown', fire, { passive: false });
    });
}

/* ═════════════════════════ ⑦ 横竖屏 ═════════════════════════
   iPad 上方向变化有三条事件路径，不同 iOS 版本触发的组合不同 ——
   只监听一条会漏，所以三条都挂上（重复触发无副作用，layout 是幂等的）。
   ⚠️ 方向变化后**必须重算映射参数**（layout），否则点哪儿都不准。
   ─────────────────────────────────────────────────────────── */
let portraitHintClosed = false;

function isPortrait() {
    /* 用 innerWidth/innerHeight 而不是 matchMedia('orientation: portrait')：
      iPad 分屏（Split View）时两者会不一致，而**可视区域比例**才是
      真正影响布局的那个量。 */
    return window.innerHeight > window.innerWidth * 1.05;
}

function checkOrientation() {
    layout();
    /* ⚠️ 这里**不能**加 `if (!running) return` ——
       竖屏提示属于"布局层"的事，与游戏有没有在跑无关。
       之前加了这一句，而 startGame 里是先调 checkOrientation()
       再置 running = true，于是竖屏提示**永远不出现**
       （本机 Playwright 实测：竖屏视口下 rotateHint 始终 hidden）。
       这类"顺序依赖"的 bug 很难靠读代码发现，必须有端到端测试。 */
    if (isPortrait() && !portraitHintClosed) {
        rotateEl.hidden = false;
    } else {
        rotateEl.hidden = true;
    }
}

function bindOrientation() {
    window.addEventListener('resize', checkOrientation);
    window.addEventListener('orientationchange', function () {
        /* orientationchange 触发时尺寸往往还没更新，延后一帧再算 */
        setTimeout(checkOrientation, 120);
        setTimeout(checkOrientation, 420);   // iOS 上有时需要更久
    });
    if (screen.orientation && screen.orientation.addEventListener) {
        screen.orientation.addEventListener('change', function () {
            setTimeout(checkOrientation, 120);
        });
    }
    if (window.visualViewport) {
        window.visualViewport.addEventListener('resize', checkOrientation);
        /* 键盘弹出 / 地址栏收起也会触发 —— 同样要重算 */
        window.visualViewport.addEventListener('scroll', checkOrientation);
    }

    $('rotateOk').addEventListener('click', function () {
        portraitHintClosed = true;
        rotateEl.hidden = true;
    });
    $('rotatePlay').addEventListener('click', function () {
        portraitHintClosed = true;
        rotateEl.hidden = true;
    });
}

/* ═════════════════════════ ⑧ 启动流程 ═════════════════════════ */
function setBootMsg(msg, pct) {
    bootMsg.textContent = msg;
    if (typeof pct === 'number') {
        bootProg.hidden = false;
        bootBar.style.width = Math.max(0, Math.min(100, pct)) + '%';
    }
}

function startGame() {
    /* 音频解锁必须在**用户手势的同步调用栈里**（iOS 要求）。
       所以这个函数由启动按钮的 click 直接调用，不做任何 async 前置。 */
    unlockAudio();

    bootEl.classList.add('gone');
    setTimeout(function () { bootEl.hidden = true; }, 400);
    keysEl.hidden = false;

    imgData = ctx2d.createImageData(DEV_W, DEV_H);
    imgU32 = new Uint32Array(imgData.data.buffer);

    layout();
    checkOrientation();

    resetSaveIfRequested();
    loadFromStorage();

    /* 开局：createLayers + 字体 + 载图 + resetGame */
    Module.ccall('gameInitAll', null, []);

    const n = Module.ccall('gameArtCount', 'number', []);
    log('素材载入 ' + n + ' 张');

    /* 立刻预热一批字形：引擎在开局排版时就把"缺哪些字"登记好了，
       这里先灌一批，避免开头零点几秒里文字是空框。
       剩下的由 frame() 每帧续补（见那里的说明）。 */
    pumpGlyphs(240);
    log('字形预热 ' + glyphCount + ' 个');

    /* 必须再同步一次存档：
       createLayers/gameBoot 里会读存档，而我们把文件写进 MEMFS 的时机
       在 gameInitAll **之前** —— 所以上面 loadFromStorage() 的位置是对的。
       但引擎在运行中会覆盖它，所以这里再存一次初始状态没意义，跳过。 */

    running = true;
    lastT = performance.now();
    requestAnimationFrame(frame);

    log('已进入游戏');
}

/* 支持 ?reset=1 清档（调试用，也方便"想重开一局"） */
function resetSaveIfRequested() {
    if (/[?&]reset=1/.test(location.search)) {
        try { localStorage.removeItem(SAVE_KEY); log('存档已按要求清空'); } catch (_) {}
    }
}

/* 从同目录加载 Emscripten 产物（pvz.js + pvz.wasm，由 Actions 构建产出）。

   ★ 加载顺序有个坑：Emscripten 生成的 js 在**执行时**就会读 window.Module，
     所以必须"先挂好 Module 配置、再把 <script> 插进 DOM"。
     之前那版写成"插进 DOM 之后在 onload 里配 Module"——
     那时 pvz.js 已经跑完了，onRuntimeInitialized 早就错过，页面永远卡在"载入中"。

   ★ 用 window.Module 而不是 MODULARIZE 工厂函数：
     Emscripten 的 pvz.js 开头是
       var Module = typeof Module != 'undefined' ? Module : {};
     预置 window.Module 就会被它直接采用，最省事、也最少出错面。
   ------------------------------------------------------------------- */
function boot() {
    bootMsg.textContent = '正在载入引擎…';
    bootHint.innerHTML = '首次进入需要下载约 50MB 素材<br>建议在 Wi-Fi 下打开';

    /* ① 先把 Module 配置挂好（此时 pvz.js 还没执行） */
    const M = {
        print:   function (t) { log(t); },
        printErr: function (t) { log('[err] ' + t); },
        onAbort: function (what) { setBootMsg('引擎异常中止：' + what, 0); },
        /* 进度：.data 包（约 50MB）的下载进度。
           没有进度反馈的话玩家会以为卡死 —— 首屏体验的关键一环。 */
        setStatus: function (t) {
            if (typeof t === 'string') {
                const m = t.match(/\(([\d.]+)\/([\d.]+)/);
                if (m && t.indexOf('Downloading') >= 0) {
                    const got = parseFloat(m[1]), tot = parseFloat(m[2]);
                    setBootMsg('正在下载素材… ' + m[1] + ' / ' + m[2] + ' MB',
                               tot > 0 ? (got / tot) * 100 : undefined);
                    return;
                }
            }
            if (t) setBootMsg(t, undefined);
        },
        monitorRunDependencies: function (left) {
            if (left > 0) setBootMsg('正在准备运行时…（剩余 ' + left + ' 项）', undefined);
        },
        onRuntimeInitialized: function () {
            Module = M;
            setBootMsg('引擎就绪', 100);
            bootBtn.disabled = false;
            bootBtn.textContent = '点击开始';
            bindPersistence();        // FS 已可用，可以开始同步存档
        }
    };
    window.Module = M;

    /* ② 再插 <script>，让 pvz.js 去执行 */
    const s = document.createElement('script');
    s.src = 'pvz.js';
    s.async = false;
    s.onerror = function () {
        setBootMsg('引擎文件 pvz.js 载入失败', 0);
        bootHint.innerHTML =
            '可能原因：<br>' +
            '· 还没构建（需要 Emscripten 产物）<br>' +
            '· 网络中断<br>' +
            '· 路径不对（pvz.js 应与 index.html 同目录）';
    };
    document.body.appendChild(s);
}

/* ═════════════════════════ 入口 ═════════════════════════ */
function main() {
    bindPointer();
    bindKeys();
    bindOrientation();
    /* ⚠️ bindPersistence 不在这里调 —— 它需要 Module.FS，
       而 FS 要等运行时初始化完才有。放到 onRuntimeInitialized 里。
       两处都调的话会注册两套定时器，存档同步跑两遍（浪费但不致命，
       所以这种重复很容易一直不被发现）。 */

    bootBtn.addEventListener('click', function () {
        if (bootBtn.disabled) return;
        startGame();
    });

    if (DEBUG) diagEl.hidden = false;

    window.addEventListener('resize', layout);
    layout();
    boot();
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', main);
} else {
    main();
}

})();
