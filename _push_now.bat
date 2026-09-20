@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo ============================================================
echo   推送代码到 GitHub  -  shaoshifu/pvc
echo ============================================================
echo.
echo 【上一次为什么没成功】
echo   弹出了登录窗口，但被取消了（日志：User canceled device code
echo   authentication）。所以代码一直没上传，公网上还看不到。
echo.
echo 【这次会看到什么】
echo   会弹出一个窗口，显示一串 8 位左右的验证码 和一个网址
echo   （通常是 https://github.com/login/device）。
echo   请：打开那个网址 - 输入验证码 - 点 Authorize。
echo   *** 不要关掉那个窗口，也不要点 Cancel ***
echo.
echo   代码约 82MB，首次推送需要几分钟，进度会在下面滚动。
echo.
pause

echo.
echo ---- 开始推送 ----
git remote set-url origin https://github.com/shaoshifu/pvc.git
git push --force-with-lease -u origin main

if errorlevel 1 goto failed

echo.
echo ============================================================
echo   推送成功
echo ============================================================
echo.
echo   接下来（各只需一次）：
echo.
echo   [1] 看构建进度（约 5-10 分钟，绿勾=成功）：
echo       https://github.com/shaoshifu/pvc/actions
echo.
echo   [2] 启用 Pages —— 这一步不做的话部署会失败：
echo       https://github.com/shaoshifu/pvc/settings/pages
echo       把 Source 改成 "GitHub Actions"，保存
echo.
echo   完成后游戏地址：
echo       https://shaoshifu.github.io/pvc/
echo.
echo   （iPad 上建议：Safari 打开 - 分享 - 添加到主屏幕）
echo.
goto end

:failed
echo.
echo ============================================================
echo   推送失败
echo ============================================================
echo.
echo   如果还是"取消认证"：重新双击本文件，这次别点 Cancel。
echo   如果提示网络错误：检查网络后重试，代码不会丢。
echo   其它报错请把上面的文字整段发给我。
echo.

:end
pause
