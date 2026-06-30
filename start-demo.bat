@echo off
setlocal

set "ROOT_DIR=%~dp0"
set "BRIDGE_DIR=%ROOT_DIR%serial-mqtt-ai-bridge"
set "WEB_DIR=%ROOT_DIR%web-dashboard"

echo ========================================
echo STM32 手语康复系统 Demo Launcher
echo ========================================
echo [INFO] root: %ROOT_DIR%

if not exist "%BRIDGE_DIR%\package.json" (
  echo [ERROR] bridge project not found: %BRIDGE_DIR%
  exit /b 1
)

if not exist "%WEB_DIR%\package.json" (
  echo [ERROR] web-dashboard project not found: %WEB_DIR%
  exit /b 1
)

netstat -ano | findstr ":8765" >nul
if %errorlevel%==0 (
  echo [WARN] port 8765 already in use, bridge may already be running
) else (
  echo [INFO] port 8765 is available
)

netstat -ano | findstr ":3000" >nul
if %errorlevel%==0 (
  echo [WARN] port 3000 already in use, web-dashboard may already be running
) else (
  echo [INFO] port 3000 is available
)

echo [INFO] starting bridge in a new window...
start "Demo Bridge" cmd /k "cd /d ""%BRIDGE_DIR%"" && npm start"
echo [OK] bridge started

echo [INFO] waiting 3 seconds for bridge warm-up...
timeout /t 3 /nobreak >nul

echo [INFO] starting web-dashboard in a new window...
start "Demo Web Dashboard" cmd /k "cd /d ""%WEB_DIR%"" && npm run dev"
echo [OK] web-dashboard started

echo [INFO] opening browser...
start "" http://localhost:3000
echo [OK] browser opened

echo ========================================
echo Demo launcher finished.
echo Bridge window and Dashboard window are running separately.
echo ========================================

endlocal
