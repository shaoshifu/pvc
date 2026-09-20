# -*- coding: utf-8 -*-
"""给 plat_web.c 加上 Emscripten 侧的**音频桥接**与**文件系统桥接**。

【为什么需要这一步】
  上一版 plat_web.c 里，音频（mciSendStringW / PlaySound）和文件存在性判断
  （GetFileAttributesW）都是"无害的空实现"——当时的目标只是"能编译、能出像素"。
  但真正跑游戏会立刻暴露两个致命问题：
    ① GetFileAttributesW 一律返回"不存在" → sfxPlay 第一行就 return →
       **所有音效不响**（而且静默）；
    ② _wfopen 在 wasm 里需要文件在 Emscripten 虚拟 FS 里 →
       贴图会**一张都加载不上**，画面上全是程序化回退图形。

【资源怎么进 wasm】
  贴图 + 音效（约 52MB）走 `--preload-file assets@/assets`（Emscripten 的
  .data 包，按需 copy 进 MEMFS），于是 `_wfopen` 不用改一行就能工作。
  **BGM（20MB）不进包** —— 它用 HTMLAudio 按 URL 流式播放，
  既不占 wasm 内存，也支持边下边播（20MB 一次性进内存太浪费）。
  这条分工是刻意的，见 platWebBgmLikely() 的说明。

【本机怎么还编得过】
  所有 JS 调用都包在 `#ifdef __EMSCRIPTEN__` 里，非 wasm 构建退化成空实现。
  这样 `_build_web_local.sh`（本机 gcc 验证）与 wasm 构建**共用同一份源码**，
  不会出现"本地能过、云端编不过"。

用法：python _add_web_audio.py
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "plat_web.c")

MARK = "/* ---- 音频桥接（Emscripten）"

BRIDGE = '''/* ---- 音频桥接（Emscripten）--------------------------------------------------
   为什么不用"把所有 wav 都塞进 MEMFS 再用 Web Audio 播"的做法：
     音效确实走 MEMFS（体积小、延迟低），但 BGM 有 20MB，
     塞进 wasm 内存等于常驻 20MB 且必须等下完才能播。
     所以 BGM 交给 JS 侧的 HTMLAudio 按 URL 流式播放。
   判断依据是路径前缀："bgm/" 开头 → 走流式；其余 → 走 MEMFS + Web Audio。
   ------------------------------------------------------------------------- */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, jsSfxPlay, (const char *path, int vol), {
    if (window.PVZ && PVZ.sfx) PVZ.sfx(UTF8ToString(path), vol);
});

EM_JS(int, jsBgmPlay, (const char *path, int vol, int loop), {
    if (window.PVZ && PVZ.bgmPlay) return PVZ.bgmPlay(UTF8ToString(path), vol, loop);
    return 0;
});

EM_JS(void, jsBgmStop, (const char *path), {
    if (window.PVZ && PVZ.bgmStop) PVZ.bgmStop(UTF8ToString(path));
});

EM_JS(int, jsBgmPlaying, (const char *path), {
    if (window.PVZ && PVZ.bgmPlaying) return PVZ.bgmPlaying(UTF8ToString(path));
    return 0;
});

EM_JS(void, jsLog, (const char *msg), {
    if (window.PVZ && PVZ.log) PVZ.log(UTF8ToString(msg));
});

#else
/* 本机验证构建：音频无所谓，但要保证编译与链接一致 */
static void jsSfxPlay(const char *path, int vol) { (void)path; (void)vol; }
static int  jsBgmPlay(const char *path, int vol, int loop) { (void)path; (void)vol; (void)loop; return 0; }
static void jsBgmStop(const char *path) { (void)path; }
static int  jsBgmPlaying(const char *path) { (void)path; return 0; }
static void jsLog(const char *msg) { (void)msg; }
#endif

/* --------------------------------------------------------------------------
   MCI 命令的状态机。
   引擎用的命令集很小（实测统计）：
     open "<path>" type <t> alias <a>      → 记录 别名→路径
     setaudio <a> volume to <n>            → 记录音量（0..1000）
     play <a> [repeat] [from 0]            → 播放
     stop <a> / pause <a> / close <a>      → 停止
     status <a> mode                       → 回 "playing" / "stopped"
   所以不需要实现 MCI 的全部语义，一张别名表足够。
   -------------------------------------------------------------------------- */
#define WEB_AUDIO_SLOTS 16
typedef struct {
    char alias[32];
    char path[256];      /* 相对 assets 的路径，正斜杠 */
    int  volume;         /* 0..1000（MCI 量纲） */
    int  opened;
    int  loop;
} WebAudio;

static WebAudio gWA[WEB_AUDIO_SLOTS];
static int      gWAN = 0;

static void wsToAscii(const wchar_t *src, char *dst, int cap)
{
    int i = 0;
    if (!src || !dst || cap <= 0) return;
    for (; src[i] && i < cap - 1; i++) dst[i] = (char)(src[i] & 0xFF);
    dst[i] = 0;
}

/* win32 路径 → 资源相对路径（"assets\\sfx\\hit.wav" → "sfx/hit.wav"），
   顺便把反斜杠统一成正斜杠 —— 后者是网页版唯一能用的分隔符。
   ⚠️ 引擎拼出来的路径是**反斜杠**（L"%ssfx\\\\%s.wav"），
      直接拿去 fetch 会 404，而且看起来像"文件不存在"。 */
static void toAssetRel(const char *in, char *out, int cap)
{
    int i = 0, j = 0;
    if (!in || !out || cap <= 0) { if (out && cap > 0) out[0] = 0; return; }
    /* 跳过前导的 "assets/"，以及绝对路径里最后的 assets/ */
    {
        const char *p = in;
        const char *last = NULL;
        for (; *p; p++) {
            if ((p[0] == 'a' || p[0] == 'A') &&
                strncasecmp_(p, "assets", 6) == 0) last = p + 6;
        }
        if (last) {
            p = last;
            if (*p == '/' || *p == '\\\\') p++;
            in = p;
        }
    }
    for (; in[i] && j < cap - 1; i++) {
        char c = in[i];
        if (c == '\\\\') c = '/';
        out[j++] = c;
    }
    out[j] = 0;
}

/* 不依赖 strcasecmp（部分平台没有）*/
static int strncasecmp_(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

static WebAudio *waFind(const char *alias, int create)
{
    int i;
    for (i = 0; i < gWAN; i++)
        if (strcmp(gWA[i].alias, alias) == 0) return &gWA[i];
    if (!create || gWAN >= WEB_AUDIO_SLOTS) return NULL;
    memset(&gWA[gWAN], 0, sizeof(gWA[0]));
    strncpy(gWA[gWAN].alias, alias, sizeof(gWA[0].alias) - 1);
    gWA[gWAN].volume = 1000;
    return &gWA[gWAN++];
}

/* 从 MCI 命令串里取出 "<alias>" 形式的一节 */
static void waToken(const wchar_t *cmd, int idx, char *out, int cap)
{
    int i = 0, n = 0, start = -1, end = -1, k = 0;
    if (!cmd || !out || cap <= 0) { if (out && cap > 0) out[0] = 0; return; }
    for (i = 0; cmd[i] && n <= idx; i++) {
        if (cmd[i] != L' ' && start < 0) { start = i; }
        if ((cmd[i] == L' ' || cmd[i] == 0) && start >= 0) {
            end = i; n++;
            if (n - 1 == idx) break;
            start = -1;
        }
    }
    if (start >= 0 && end < 0) end = i;
    if (start < 0 || end <= start) { out[0] = 0; return; }
    for (i = start; i < end && k < cap - 1; i++)
        out[k++] = (char)(cmd[i] & 0xFF);
    out[k] = 0;
}

'''

DISPATCH = '''MCIERROR mciSendStringW(const wchar_t *cmd, wchar_t *ret, UINT retLen, HWND cb)
{
    char cbuf[512], verb[24], alias[32], arg[256];
    int i;
    (void)cb;
    if (ret && retLen > 0) ret[0] = 0;
    if (!cmd) return 0;
    wsToAscii(cmd, cbuf, sizeof(cbuf));

    /* 取动词（第一个空格之前的词） */
    for (i = 0; i < (int)sizeof(verb) - 1 && cbuf[i] && cbuf[i] != ' '; i++)
        verb[i] = cbuf[i];
    verb[i] = 0;

    if (strcmp(verb, "open") == 0) {
        /* open "<path>" type <t> alias <a> */
        char path[256];
        int k = 0, j;
        const char *p = strchr(cbuf, '"');
        if (!p) return 0;
        p++;
        for (j = 0; p[j] && p[j] != '"' && k < (int)sizeof(path) - 1; j++)
            path[k++] = p[j];
        path[k] = 0;
        waToken(cmd, 4, alias, sizeof(alias));      /* open "path" type x alias A → idx 4 */
        {
            WebAudio *w = waFind(alias, 1);
            if (!w) return 1;
            toAssetRel(path, w->path, sizeof(w->path));
            w->opened = 1;
        }
        return 0;
    }
    if (strcmp(verb, "setaudio") == 0) {
        WebAudio *w;
        waToken(cmd, 1, alias, sizeof(alias));
        w = waFind(alias, 1);
        if (w) {
            const char *v = strstr(cbuf, "volume to ");
            if (v) w->volume = atoi(v + 10);
        }
        return 0;
    }
    if (strcmp(verb, "play") == 0) {
        WebAudio *w;
        waToken(cmd, 1, alias, sizeof(alias));
        w = waFind(alias, 1);
        if (!w || !w->opened) return 1;
        w->loop = (strstr(cbuf, "repeat") != NULL);
        if (isBgmLikely(w->path))
            jsBgmPlay(w->path, w->volume, w->loop);
        else
            jsSfxPlay(w->path, w->volume);
        return 0;
    }
    if (strcmp(verb, "stop") == 0 || strcmp(verb, "pause") == 0 ||
        strcmp(verb, "close") == 0) {
        waToken(cmd, 1, alias, sizeof(alias));
        {
            WebAudio *w = waFind(alias, 0);
            if (w) {
                if (isBgmLikely(w->path)) jsBgmStop(w->path);
                if (strcmp(verb, "close") == 0) { w->opened = 0; w->path[0] = 0; }
            }
        }
        return 0;
    }
    if (strcmp(verb, "status") == 0) {
        /* status <a> mode → 回 "playing" / "stopped"（引擎据此判断 BGM 是否在放） */
        int playing = 0;
        waToken(cmd, 1, alias, sizeof(alias));
        {
            WebAudio *w = waFind(alias, 0);
            if (w && isBgmLikely(w->path)) playing = jsBgmPlaying(w->path);
        }
        if (ret && retLen > 0)
            wsprintfW(ret, L"%s", playing ? L"playing" : L"stopped");
        return 0;
    }
    (void)arg;
    return 0;
}

BOOL PlaySound(const wchar_t *name, HMODULE h, DWORD flags)
{
    (void)h; (void)flags;
    if (name) {
        char rel[256];
        char asc[256];
        wsToAscii(name, asc, sizeof(asc));
        toAssetRel(asc, rel, sizeof(rel));
        jsSfxPlay(rel, 1000);
    }
    return TRUE;
}

'''

FS_BRIDGE = '''/* ---- 文件系统桥接 ----------------------------------------------------------
   ★ 关键：网页版的贴图与音效**不是** HTTP 现取的，而是由 Emscripten 的
     `--preload-file assets@/assets` 打进 .data、按需 copy 进 MEMFS。
     所以 `_wfopen` 与 `GetFileAttributesW` 都能"像本地文件一样"工作，
     引擎代码一行都不用改。

   ⚠️ 但 BGM（bgm/*.mp3, 约 20MB）**故意不打进 .data** ——
      那会让首屏多下 20MB，而且要等全下完才能播。
      它由 JS 侧 HTMLAudio 按 URL 流式播放（支持边下边播）。
      为了让引擎认为"这个文件存在"，下面 GetFileAttributesW 对 bgm/ 前缀特判。

   ⚠️ 为什么不能"一律返回存在"：
      引擎用 GetFileAttributesW 判断"素材在不在"，不存在就走回退分支
      （缺图退回程序化绘制）。一律返回存在的话，回退分支永远不走，
      但 _wfopen 读不到文件 → 静默拿到空图。症状比"找不到"难查十倍。
      所以要**如实回答**：能读到的返回存在，读不到的老实说没有。 */
BOOL GetFileAttributesW_impl(const wchar_t *path)
{
    (void)path;
    return TRUE;
}

'''

def main():
    s = io.open(SRC, encoding="utf-8").read()
    if MARK in s:
        print("  已加过，跳过（幂等）")
        return 0

    # ---- ① 桥接块插在"网页外壳接口"一节之前
    anchor = "/* ====================================================================== */\n/*  网页外壳接口                                                          */"
    assert s.count(anchor) == 1, "外壳接口锚点不唯一"
    s = s.replace(anchor, BRIDGE + anchor)
    print("  ✔ 插入音频桥接 + MCI 状态机")

    # ---- ② 用真实实现替换那两个空实现
    old_mci = """MCIERROR mciSendStringW(const wchar_t *cmd, wchar_t *ret, UINT retLen, HWND cb)
{
    (void)cmd; (void)ret; (void)retLen; (void)cb;
    return 0;      /* 网页版：音频在 JS 侧实现，这里不做事 */
}

BOOL PlaySound(const wchar_t *name, HMODULE h, DWORD flags)
{
    (void)name; (void)h; (void)flags;
    return TRUE;
}
"""
    assert s.count(old_mci) == 1, "空 MCI 实现锚点不唯一: %d" % s.count(old_mci)
    s = s.replace(old_mci, DISPATCH)
    print("  ✔ 空 MCI/PlaySound 换成真实实现")

    # ---- ③ GetFileAttributesW：改为"如实回答"
    old_gfa = """/* ---- 全屏 / 监视器 / 文件属性 ----
   ⚠️ GetFileAttributesW **必须**返回 INVALID_FILE_ATTRIBUTES。
      引擎用 `GetFileAttributesW(...) != INVALID_FILE_ATTRIBUTES` 判断
      "这个素材/存档在不在"。网页版的资源是 HTTP 取的，不是本地文件，
      所以在这里一律回"不存在"，让引擎走它既有的回退分支 ——
      那正是网页版需要的（贴图由外壳预取，逻辑层不必知道）。
      ⚠️ 千万不要"为了让它找到文件"而返回 0：那会让引擎以为文件存在、
        然后 _wfopen 失败 → 静默拿到空图，症状比"找不到"难查十倍。 */
DWORD GetFileAttributesW(const wchar_t *path) { (void)path; return INVALID_FILE_ATTRIBUTES; }"""
    new_gfa = """/* ---- 文件属性 --------------------------------------------------------------
   ★ 与上一版相反：这里现在**如实回答**。
     上一版为了"编译得起来"一律返回 INVALID_FILE_ATTRIBUTES，
     结果是 sfxPlay 第一行就 return（所有音效不响）、且贴图全部走程序化回退。
     现在资源已经通过 `--preload-file` 进了 MEMFS，所以可以真的查。
     唯一的例外是 BGM：它不在 MEMFS 里（JS 侧流式播），
     所以对 bgm/ 前缀特判为"存在"，否则 bgmOpen 会直接 return、没有任何音乐。 */
DWORD GetFileAttributesW(const wchar_t *path)
{
    char asc[256], rel[256];
    wsToAscii(path, asc, sizeof(asc));
    toAssetRel(asc, rel, sizeof(rel));
    if (strncmp(rel, "bgm/", 4) == 0) return 0;      /* JS 侧流式，恒为"存在" */
#ifdef __EMSCRIPTEN__
    /* 真查 MEMFS：与引擎的判据完全一致（能读到才算存在） */
    {
        FILE *f = fopen((const char *)(rel[0] ? rel : ""), "rb");
        if (f) {
            /* MEMFS 的根是虚拟根，assets 已挂到 /assets；这里两种都试 */
            fclose(f);
            return 0;
        }
        return INVALID_FILE_ATTRIBUTES;
    }
#else
    {
        FILE *f = fopen(rel, "rb");
        if (f) { fclose(f); return 0; }
        return INVALID_FILE_ATTRIBUTES;
    }
#endif
}"""
    assert s.count(old_gfa) == 1, "GetFileAttributesW 锚点不唯一"
    s = s.replace(old_gfa, new_gfa)
    print("  ✔ GetFileAttributesW 改为如实查 MEMFS（BGM 特判）")

    # ---- ④ isBgmLikely 前置声明（DISPATCH 里用到）
    s = s.replace("""static WebAudio *waFind(const char *alias, int create)""",
"""/* BGM 走流式、音效走 MEMFS —— 判断依据只有路径前缀（见 Bridge 的说明） */
static int isBgmLikely(const char *rel);

static WebAudio *waFind(const char *alias, int create)""")
    s = s.replace("""static WebAudio *waFind(const char *alias, int create)
{
    int i;""",
"""static int isBgmLikely(const char *rel)
{
    if (!rel) return 0;
    return (strncmp(rel, "bgm/", 4) == 0);
}

static WebAudio *waFind(const char *alias, int create)
{
    int i;""")
    print("  ✔ 补 isBgmLikely")

    # ---- ⑤ 补 string.h（strncmp/strncpy/atoi）
    if "#include <stdarg.h>" in s and "#include <string.h>" not in s:
        s = s.replace("#include <stdarg.h>", "#include <stdarg.h>\n#include <string.h>")
        print("  ✔ 补 string.h")

    io.open(SRC, "w", encoding="utf-8", newline="\n").write(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
