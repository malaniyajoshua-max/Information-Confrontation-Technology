@echo off
setlocal

set "VS_VARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VS_VARS%" (
    echo Visual Studio 2022 vcvars64.bat was not found.
    exit /b 1
)

call "%VS_VARS%" > nul
if errorlevel 1 exit /b 1

if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 src\common.cpp src\server.cpp /Febuild\remote_lab_server.exe /link ws2_32.lib gdi32.lib user32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 src\common.cpp src\client.cpp /Febuild\remote_lab_client.exe /link ws2_32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 src\common.cpp src\server_gui.cpp /Febuild\remote_lab_server_gui.exe /link /SUBSYSTEM:WINDOWS ws2_32.lib gdi32.lib user32.lib comdlg32.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 src\common.cpp src\client_gui.cpp /Febuild\remote_lab_client_gui.exe /link /SUBSYSTEM:WINDOWS ws2_32.lib user32.lib comdlg32.lib
if errorlevel 1 exit /b 1

echo Build completed:
echo   build\remote_lab_server.exe
echo   build\remote_lab_client.exe
echo   build\remote_lab_server_gui.exe
echo   build\remote_lab_client_gui.exe
