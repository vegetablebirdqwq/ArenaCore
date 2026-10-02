@echo off
chcp 65001 >nul
REM ============================================================
REM ArenaCore one-click demo: server + 6 bots 3v3
REM Usage: double-click this file (or run run_demo.cmd in cmd)
REM ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo  ArenaCore 3v3 demo
echo  1. 启动服务器 (9527)
echo  2. 连 6 个 bot 打一局
echo  3. 观察服务器日志（快照广播）
echo ============================================================

REM 清理残留
taskkill /IM game_server.exe /F >nul 2>&1
timeout /t 1 /nobreak >nul

REM 启动服务器（后台）
start "ArenaCore-Server" cmd /k "build\game_server.exe"

REM 等服务器就绪
timeout /t 2 /nobreak >nul

REM 服务器已预填 4 个 bot，再连 2 个真实客户端就满 6 人开打
start "Bot1" cmd /k "build\bot_client.exe 1"
start "Bot2" cmd /k "build\bot_client.exe 2"

echo.
echo 已启动 2 个 bot（+ 服务器预填 4 个 = 满 6 人开打）。
echo.
echo 打开监控页看技术过程 + 实时日志：
echo   start monitor.html
echo.
echo 服务器窗口日志：玩家加入 -^> 满 6 人开打 -^> 每秒广播汇总
echo 120 秒后服务器自动退出（Ctrl+C 可提前结束）。
echo.
endlocal
