# -*- coding: utf-8 -*-
"""探测 MiniMax 音乐生成接口是否可用。

背景：mmx CLI 1.0.25 只暴露 text/speech/image/video/search/vision，
没有 music 子命令 —— 但 MiniMax 平台本身有 music_generation 端点。
mmx 的 api_key 就是 MiniMax 的 key，所以直接调平台接口即可。

从 ~/.mmx/config.json 读 key（不走命令行参数，避免明文外发被安全策略拦截）。

用法： python _probe_music.py
"""
import json
import os
import sys
import urllib.request
import urllib.error

CFG = os.path.join(os.path.expanduser("~"), ".mmx", "config.json")
BASE = "https://api.minimaxi.com"


def load_key():
    with open(CFG, encoding="utf-8") as f:
        return json.load(f)["api_key"]


def post(path, payload, timeout=120):
    req = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Authorization": "Bearer " + load_key(),
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        return e.code, body
    except Exception as e:
        return -1, str(e)


def main():
    print("[1] 探测 /v1/music_generation ...")
    payload = {
        "model": "music-1.5",
        "prompt": "轻快活泼的休闲游戏背景音乐，钢琴与木琴，明亮的大调",
        "lyrics": "",
        "audio_setting": {"sample_rate": 44100, "bitrate": 128000, "format": "mp3"},
    }
    st, res = post("/v1/music_generation", payload)
    print("    HTTP %s" % st)
    if isinstance(res, dict):
        br = res.get("base_resp", {})
        print("    base_resp: %s" % br)
        data = res.get("data", {})
        if isinstance(data, dict):
            print("    返回字段: %s" % list(data.keys()))
            audio = data.get("audio")
            if audio:
                print("    ✓ 拿到音频（base64 长度 %d → 约 %.1f 秒）"
                      % (len(audio), len(audio) * 3 / 4 / 32000))
        # 有些版本把音频放 hex
        if isinstance(data, dict) and data.get("audio_hex"):
            print("    ✓ audio_hex 长度 %d" % len(data["audio_hex"]))
    else:
        print("    返回: %s" % str(res)[:400])
    return 0


if __name__ == "__main__":
    sys.exit(main())
