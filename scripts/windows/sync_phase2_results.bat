@echo off
setlocal

rem 从 scripts\windows 目录切回 OrderSystem 项目根目录。
cd /d "%~dp0..\.."

echo ============================================================
echo OrderSystem Phase 2 Test Result Sync
echo ============================================================
echo.
echo Linux:
echo   /home/yjj/rpc-bench/ordersystem_phase2_async/
echo.
echo Windows:
echo   perf-results\phase2_async\
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\scripts\windows\sync_phase2_results.ps1"

set "EXIT_CODE=%ERRORLEVEL%"

echo.
if "%EXIT_CODE%"=="0" (
    echo Sync completed.
) else (
    echo Sync failed. Exit code: %EXIT_CODE%
)

echo.
pause
exit /b %EXIT_CODE%
