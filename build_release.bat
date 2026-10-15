@echo off
setlocal
echo ========================================
echo   Compilando VMT-Studio (Release)
echo ========================================
powershell -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -Config Release
if errorlevel 1 (
    echo.
    echo [ERROR] La compilacion de Release ha fallado.
    pause
    exit /b 1
)
echo.
echo [OK] Compilacion Release completada con exito.
pause
