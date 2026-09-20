#!/usr/bin/env bash
# =============================================================================
#  _ci_post_log.sh —— 把一段 CI 日志贴到 **commit 评论**里
#
#  【为什么需要它】
#    GitHub Actions 的原始日志要认证才能下载：匿名访问
#    `/repos/{owner}/{repo}/actions/jobs/{id}/logs` 返回 **403**。
#    于是出现一个死循环：CI 失败 → 我看不到原因 → 只能猜 → 改一版再失败。
#    而 **公开仓库的 commit 评论无需登录就能读**。
#    所以让 CI 自己把日志尾部贴到评论里，这个循环就断了 ——
#    我（以及任何协作者、甚至没登录的访客）都能直接看到真实报错。
#
#  【用法】在 workflow 的步骤里：
#      if ! <你的命令> > /tmp/step.log 2>&1; then
#          cat /tmp/step.log
#          bash _ci_post_log.sh /tmp/step.log "步骤名" || true
#          exit 1
#      fi
#
#  环境变量（Actions 里默认都有）：
#      GITHUB_TOKEN       —— 由 workflow 的 permissions 提供
#      GITHUB_REPOSITORY  —— owner/repo
#      GITHUB_SHA         —— 当前提交
#  另可用 TITLE 传标题，或作为第 2 个参数。
#
#  ⚠️ 只保留尾部 4000 字符：GitHub 单条评论上限 65536，
#     而编译错误通常在最后（前面全是无关的进度输出）。
# =============================================================================
set -u

LOG="${1:-/tmp/step.log}"
TITLE="${2:-CI 步骤失败}"

if [ ! -f "$LOG" ]; then
    echo "  （日志文件 $LOG 不存在，跳过上报）"
    exit 0
fi
if [ -z "${GITHUB_TOKEN:-}" ] || [ -z "${GITHUB_REPOSITORY:-}" ]; then
    echo "  （缺 GITHUB_TOKEN / GITHUB_REPOSITORY，跳过上报）"
    exit 0
fi

TAIL=$(tail -c 4000 "$LOG")
BODY=$(TITLE="$TITLE" TAIL="$TAIL" python3 - <<'PY'
import json, os
title = os.environ.get("TITLE", "CI 步骤失败")
tail = os.environ.get("TAIL", "")
print(json.dumps("**%s**\n\n```\n%s\n```" % (title, tail)))
PY
)

HTTP=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    -H "Authorization: token ${GITHUB_TOKEN}" \
    -H "Accept: application/vnd.github+json" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    "https://api.github.com/repos/${GITHUB_REPOSITORY}/commits/${GITHUB_SHA:-HEAD}/comments" \
    --data "$BODY" 2>/dev/null)

if [ "$HTTP" = "201" ]; then
    echo "  ✓ 失败日志已贴到 commit 评论（无需登录即可读）"
else
    echo "  （贴评论返回 HTTP $HTTP，不影响退出码）"
fi
exit 0
