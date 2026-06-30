@echo off
setlocal

cd /d %~dp0

set "ROOT_DIR=%~dp0"
set "BRIDGE_DIR=%ROOT_DIR%serial-mqtt-ai-bridge"
set "WEB_DIR=%ROOT_DIR%web-dashboard"

echo ========================================
echo STM32 Demo Launcher
echo ========================================
echo [INFO] root: %ROOT_DIR%

where npm >nul 2>nul
if errorlevel 1 (
  echo [ERROR] npm not found in PATH
  pause
  exit /b 1
)

if not exist "%BRIDGE_DIR%\package.json" (
  echo [ERROR] bridge project not found: %BRIDGE_DIR%
  pause
  exit /b 1
)

if not exist "%WEB_DIR%\package.json" (
  echo [ERROR] web-dashboard project not found: %WEB_DIR%
  pause
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

echo [INFO] starting bridge...
start "Demo Bridge" cmd /k "cd /d ""%BRIDGE_DIR%"" && npm start"
if errorlevel 1 (
  echo [ERROR] failed to start bridge window
  pause
  exit /b 1
)
echo [OK] bridge started

echo [INFO] waiting 3 seconds for bridge warm-up...
timeout /t 3 /nobreak >nul

echo [INFO] starting web...
start "Demo Web Dashboard" cmd /k "cd /d ""%WEB_DIR%"" && npm run dev"
if errorlevel 1 (
  echo [ERROR] failed to start web-dashboard window
  pause
  exit /b 1
)
echo [OK] web-dashboard started

echo [INFO] opening browser...
start "" http://localhost:3000
if errorlevel 1 (
  echo [ERROR] failed to open browser
  pause
  exit /b 1
)
echo [OK] browser opened

echo ========================================
echo [OK] done
echo Bridge window and Dashboard window are running separately.
echo ========================================

pause
endlocal
