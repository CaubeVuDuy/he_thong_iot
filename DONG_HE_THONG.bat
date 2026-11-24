@echo off
title DONG HE THONG IOT
chcp 65001 >nul

echo Dang tat tien trinh Node.js (Server & MQTT Broker)...
taskkill /F /IM node.exe >nul 2>&1

echo Dang tat YOLO AI Worker (cong 8001)...
for /f "tokens=5" %%a in ('netstat -ano ^| findstr :8001 ^| findstr LISTENING') do taskkill /F /PID %%a >nul 2>&1
taskkill /F /FI "WINDOWTITLE eq YOLO_AI_WORKER*" >nul 2>&1

echo Da tat he thong thanh cong!
timeout /t 2 >nul
exit
