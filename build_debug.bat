@echo off
setlocal
echo ========================================
echo   Compilando VMT-Studio (Debug)
echo ========================================
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -Config Debug
if errorlevel 1 (
    echo.
    echo [ERROR] La compilacion de Debug ha fallado.
    pause
    exit /b 1
)
echo.
echo [OK] Compilacion Debug completada con exito.
pause
