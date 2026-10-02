@echo off
REM ============================================================
REM ArenaCore 3v3 demo (ASCII only - cmd cannot parse UTF-8 chinese)
REM Server runs in THIS window, bots open new windows.
REM ============================================================
setlocal
cd /d "%~dp0"

echo ============================================================
echo  ArenaCore 3v3 demo
echo  Server runs here for 120s (Ctrl+C to stop early)
echo  2 Bot windows will pop up (2 bots + 4 builtin = 6 players)
echo  For code walkthrough: open watch-and-learn.html
echo ============================================================
echo.

REM kill leftovers
taskkill /IM game_server.exe /F >nul 2>&1
taskkill /IM bot_client.exe /F >nul 2>&1
timeout /t 1 /nobreak >nul

REM start 2 bots first (they auto-retry until server is up)
REM bot1 = default mode (Wanderer), bot2 = mode 2 (Aggressor) - different behaviors
start "Bot1-Wanderer" cmd /k "build\bot_client.exe 1"
start "Bot2-Aggressor" cmd /k "build\bot_client.exe 2 2"

REM server runs in foreground (log shows here)
echo [demo] starting server...
timeout /t 1 /nobreak >nul
build\game_server.exe

echo.
echo [demo] server exited.
pause
endlocal
