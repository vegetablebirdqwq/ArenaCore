@echo off
chcp 65001 >nul
REM ============================================================
REM ArenaCore 3v3 demo
REM 服务器在本窗口前台跑（日志直接看），bot 开新窗口
REM ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo  ArenaCore 3v3 demo
echo  服务器在本窗口跑 120 秒（Ctrl+C 可提前结束）
echo  Bot 窗口会自动弹出（连 2 个 bot + 服务器预填 4 个 = 6 人）
echo  想看技术对照：另开 monitor.html 和 watch-and-learn.html
echo ============================================================
echo.

REM 清理残留
taskkill /IM game_server.exe /F >nul 2>&1
taskkill /IM bot_client.exe /F >nul 2>&1
timeout /t 1 /nobreak >nul

REM 先开 2 个 bot 窗口（它们会自动重试连接，等服务器起来）
start "Bot1" cmd /k "build\bot_client.exe 1"
start "Bot2" cmd /k "build\bot_client.exe 2"

REM 服务器前台跑（占住本窗口，日志直接显示；2 秒后 bot 连上满 6 人开打）
echo [demo] 启动服务器...
timeout /t 1 /nobreak >nul
build\game_server.exe

REM 服务器退出后到这里
echo.
echo [demo] 服务器已退出。
pause
endlocal
