@echo off
setlocal
cd /d "%~dp0"
python -B tools\update_core_server.py %*
if errorlevel 1 (echo Core/Server update FAILED.& pause & exit /b 1)
echo Core/Server update completed.
pause
