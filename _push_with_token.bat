@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

echo ============================================================
echo   推送代码到 GitHub  -  shaoshifu/pvc
echo ============================================================
echo.
echo 【第 1 步】浏览器打开下面这个网址（登录你的 GitHub 账号）：
echo.
echo     https://github.com/settings/tokens/new
echo.
echo   ※ 如果上面这个打不开，用这个：
echo     https://github.com/settings/tokens  然后点 "Generate new token (classic)"
echo.
echo 【第 2 步】在页面上填三处：
echo.
echo     Note（备注）    ：随便写，比如  pvz-web
echo     Expiration      ：选 30 days
echo     Select scopes   ：勾选下面两个方框
echo                         [v]  repo        （最上面那个大项，勾它会把子项全勾上）
echo                         [v]  workflow    （在列表中间，必须勾！
echo                                            不勾的话含 .github/workflows 的代码会被拒）
echo.
echo     然后拉到页面最底部，点绿色的 "Generate token"
echo.
echo 【第 3 步】页面会显示一串以 ghp_ 开头的字符。点它右边的复制按钮。
echo.
echo 【第 4 步】回到这个窗口，粘贴（在窗口里点右键就是粘贴），然后按回车。
echo.
echo ============================================================
echo.
pause

echo.
set /p TOKEN=请粘贴令牌后按回车:
if "%TOKEN%"=="" (
  echo.
  echo   没有输入内容，退出。请重新双击本文件。
  goto end
)

echo.
echo ---- 正在推送，代码约 82MB，请耐心等待 ----
echo.

git remote set-url origin https://x-access-token:%TOKEN%@github.com/shaoshifu/pvc.git
git fetch origin main 2>nul
git push --force-with-lease -u origin main

set PUSHOK=%errorlevel%

rem 无论成败都把令牌从 remote 地址里清掉，不留在配置文件里
git remote set-url origin https://github.com/shaoshifu/pvc.git

if not "%PUSHOK%"=="0" goto failed

echo.
echo ============================================================
echo   推送成功
echo ============================================================
echo.
echo   接下来两件事（各只需做一次）：
echo.
echo   [1] 看构建进度，约 5-10 分钟，出现绿勾就是成功：
echo       https://github.com/shaoshifu/pvc/actions
echo.
echo   [2] 启用 Pages，不做这一步部署会失败：
echo       https://github.com/shaoshifu/pvc/settings/pages
echo       把 Source 那一栏改成  "GitHub Actions"  然后点 Save
echo.
echo   全部完成后，游戏就在这个地址：
echo       https://shaoshifu.github.io/pvc/
echo.
echo   iPad 上打开后建议：点分享按钮 - 添加到主屏幕
echo.
goto end

:failed
echo.
echo ============================================================
echo   推送失败（错误码 %PUSHOK%）
echo ============================================================
echo.
echo   常见原因：
echo     · 令牌勾选不全 -- 必须同时勾 repo 和 workflow，重新生成一个
echo     · 令牌复制不全 -- 重新复制一次，注意开头是 ghp_
echo     · 令牌已过期   -- 重新生成
echo     · 网络中断     -- 检查网络后重试，代码不会丢
echo.
echo   如果还不行，把上面的文字整段发给我。
echo.

:end
echo.
pause
endlocal
