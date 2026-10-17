@echo off
setlocal
if "%~1"=="" (
  echo Supply a separate output directory for the geometry probe.
  exit /b 2
)
set "curvedOutput=%~1"
if not exist "%curvedOutput%" mkdir "%curvedOutput%"
if errorlevel 1 exit /b 1
call "C:\VibeCADTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 "%~dp0probe.cpp" /Fo"%curvedOutput%\curved_transition_probe.obj" /Fe"%curvedOutput%\curved_transition_probe.exe"
exit /b %errorlevel%
