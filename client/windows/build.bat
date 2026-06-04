@echo off

:: Build Console Client
echo === Building console client ===
where cl >nul 2>&1
if %ERRORLEVEL% == 0 (
    cl /std:c++17 /O2 /EHsc /W3 ns-gamepad.cpp /link ws2_32.lib xinput.lib winmm.lib user32.lib /out:ns-gamepad.exe
    goto :gui
)

where g++ >nul 2>&1
if %ERRORLEVEL% == 0 (
    g++ -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput -lwinmm -luser32
    goto :gui
)

if exist "C:\msys64\ucrt64\bin\g++.exe" (
    C:\msys64\ucrt64\bin\g++.exe -std=c++17 -O2 -Wall ns-gamepad.cpp -o ns-gamepad.exe -static -lws2_32 -lxinput -lwinmm -luser32
)

:gui
echo === Building GUI app ===
where cl >nul 2>&1
if %ERRORLEVEL% == 0 (
    if exist icon.ico (
        rc /nologo ns-gui.rc
        cl /std:c++17 /O2 /EHsc /W3 ns-gui.cpp ns-gui.res /link ws2_32.lib xinput.lib setupapi.lib comctl32.lib gdiplus.lib user32.lib kernel32.lib gdi32.lib advapi32.lib winmm.lib /out:ns-gui.exe /SUBSYSTEM:WINDOWS
    ) else (
        cl /std:c++17 /O2 /EHsc /W3 ns-gui.cpp /link ws2_32.lib xinput.lib setupapi.lib comctl32.lib gdiplus.lib user32.lib kernel32.lib gdi32.lib advapi32.lib /out:ns-gui.exe /SUBSYSTEM:WINDOWS
    )
    goto :end
)

if exist "C:\msys64\ucrt64\bin\g++.exe" (
    echo MinGW GUI build not yet supported - use MSVC or run with Python UI
)

:end
echo Done.
pause
