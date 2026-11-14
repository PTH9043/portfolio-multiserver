@echo off
setlocal
cd /d "%~dp0"
python -B tools\build_protocol.py %*
if errorlevel 1 (echo Protocol build FAILED.& pause & exit /b 1)
echo Protocol build completed.
pause
