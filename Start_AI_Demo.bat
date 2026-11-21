@echo off
chcp 65001 >nul
setlocal
set ROOT=%~dp0
set SERVER_DIR=%ROOT%Server\Bin\Release
set CLIENT_DIR=%ROOT%..\Client\bin\Release\net10.0-windows

echo [1/4] MultiServer (127.0.0.1:7777, --trace=stream) 실행...
start "MultiServer (ServerApp)" /D "%SERVER_DIR%" cmd /k "%SERVER_DIR%\ServerApp.exe 4 7777 --trace=stream"

timeout /t 2 /nobreak >nul

echo [2/4] 이종 멀티모델 AI Agent 4인 + 실시간 서버 계측 HUD (http://127.0.0.1:8766/) 실행...
start "AI Agents + Live Telemetry HUD" /D "%ROOT%tools" cmd /k "python %ROOT%tools\ai_chat_agents.py --port 7777 --hud-port 8766 --banter-turns 3 --turn-pause 1.4"

timeout /t 2 /nobreak >nul

echo [3/4] C# WinForms ChatClient 실행...
start "ChatClient" /D "%CLIENT_DIR%" "%CLIENT_DIR%\ChatClient.exe"

echo [4/4] 실시간 계측 대시보드 브라우저 열기...
start "" "http://127.0.0.1:8766/"

echo 준비 완료! 좌측에 ChatClient, 우측에 실시간 계측 HUD를 배치하고 녹화를 시작하세요.
endlocal
