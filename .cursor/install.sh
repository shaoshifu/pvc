#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════
#  Cloud Agent 安装脚本 —— 让 Linux 上能构建并运行「网页版（WASM）」。
#
#  仓库同一份 pvz.c 编两个平台：Windows 桌面版（需 Win32 GDI/D3D9，Linux 上
#  无法构建），以及网页版（Emscripten → WebAssembly）。Cloud Agent 是 Linux，
#  所以这里只准备网页版，它也是能端到端演示、可玩的目标。
#
#  这个脚本做两件事，顺序刻意如此：
#    ① 安装并激活 Emscripten 3.1.61（与 .github/workflows/build-web.yml 对齐）。
#       默认镜像不自带 emcc，工具链要从 storage.googleapis.com 下载。
#    ② 用 emcc 把 pvz.c + plat_web.c 编成 web/pvz.{js,wasm,data}
#       （flags 与 CI 完全一致；改动前先读 CI 里那份逐条注释）。
#
#  幂等：emsdk 已装好则跳过重复下载；构建产物每次覆盖重建。
# ═══════════════════════════════════════════════════════════════════════
set -euo pipefail

EMSDK_VERSION="3.1.61"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "== ① 安装 / 激活 Emscripten ${EMSDK_VERSION} =="
if [ ! -d "$EMSDK_DIR/.git" ]; then
  rm -rf "$EMSDK_DIR"
  git clone https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi
cd "$EMSDK_DIR"
./emsdk install "$EMSDK_VERSION"
./emsdk activate "$EMSDK_VERSION"
# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh"
emcc --version | head -1

echo "== ② 核对资源 =="
cd "$REPO_DIR"
python3 _verify_assets.py

echo "== ③ 编译网页版 WASM =="
# flags 逐条含义见 .github/workflows/build-web.yml 的「编译 WASM」步骤。
emcc -DPLAT_PORTABLE -O2 -I. \
  -sWASM=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFORCE_FILESYSTEM=1 \
  -sINITIAL_MEMORY=67108864 \
  -sSTACK_SIZE=5242880 \
  -sEXPORTED_FUNCTIONS='["_gameInitAll","_gameStep","_gamePointerMove","_gamePointerDown","_gamePointerUp","_handleKey","_gameArtCount","_platWebFrame","_platWebInit","_gameSetAssetDir","_gameSetDeterministic","_platWebGlyph","_platWebGlyphReq","_platWebGlyphCount","_platWebGlyphReqCount","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall","cwrap","FS","HEAPU32","HEAPU8","HEAPF32"]' \
  --preload-file assets@/assets \
  --no-entry \
  -o web/pvz.js \
  pvz.c plat_web.c

# Pages 默认会跑 Jekyll 并忽略下划线开头的文件；直接跳过（与 CI 一致）。
touch web/.nojekyll

echo "== 产物 =="
ls -lh web/pvz.js web/pvz.wasm web/pvz.data
echo "✓ 网页版构建完成"
