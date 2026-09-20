@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo ============================================================
echo   推送代码到 GitHub（shaoshifu/pvc）
echo ============================================================
echo.
echo 如果弹出登录窗口，请选择 "Sign in with your browser"
echo 然后在浏览器里完成 GitHub 授权（只需这一次）。
echo.
echo 代码约 82MB，首次推送需要几分钟，请耐心等待。
echo.
pause

git push -u origin main

if errorlevel 1 (
  echo.
  echo ------------------------------------------------------------
  echo 推送失败。常见原因：
  echo   1. 登录被取消 -- 重新双击本文件再试一次
  echo   2. 网络中断   -- 检查网络后重试
  echo ------------------------------------------------------------
) else (
  echo.
  echo ============================================================
  echo   推送成功！接下来两件事：
  echo ============================================================
  echo.
  echo   [1] 看构建进度（约 5-10 分钟）：
  echo       https://github.com/shaoshifu/pvc/actions
  echo.
  echo   [2] 启用 Pages（只需一次，否则部署步骤会失败）：
  echo       https://github.com/shaoshifu/pvc/settings/pages
  echo       把 "Source" 设为 "GitHub Actions"
  echo.
  echo   完成后游戏地址：
  echo       https://shaoshifu.github.io/pvc/
  echo.
)

pause
