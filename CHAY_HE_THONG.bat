@echo off
title HE THONG IOT VIEN DUONG LAO
chcp 65001 >nul

echo ===================================================
echo     DANG KHOI DONG HE THONG IOT VIEN DUONG LAO
echo ===================================================

netstat -ano | findstr :8001 | findstr LISTENING >nul
if %errorlevel% neq 0 (
    if exist "d:\iot final\ai_worker\worker.py" (
        echo [1/3] Dang khoi dong YOLO AI Worker tren GPU/PC...
        start "YOLO_AI_WORKER" /min cmd /c "cd /d "d:\iot final\ai_worker" && python worker.py"
    )
) else (
    echo [1/3] YOLO AI Worker da dang chay!
)

cd /d "d:\iot final\backend"

netstat -ano | findstr :3000 | findstr LISTENING >nul
if %errorlevel% neq 0 (
    echo [2/3] Dang khoi dong Backend & MQTT Broker...
    start "IOT_BACKEND_SERVER" /min cmd /c "node server.js"
    timeout /t 2 /nobreak >nul
) else (
    echo [2/3] Backend & MQTT Broker da dang chay!
)

echo [3/3] Dang mo Web Dashboard tren trinh duyet...
start http://localhost:3000

echo.
echo Da khoi dong xong! Cua so nay se tu dong dong sau 3 giay.
timeout /t 3 >nul
exit
