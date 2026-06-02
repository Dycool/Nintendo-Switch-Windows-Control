@echo off

where cl >nul 2>&1
if %ERRORLEVEL% == 0 (
    cl /std:c++17 /O2 /EHsc /W3 ns-gamepad.cpp /link ws2_32.lib xinput.lib /out:gamepad.exe
    goto :done
)

where g++ >nul 2>&1
if %ERRORLEVEL% == 0 (
    g++ -std=c++17 -O2 -Wall ns-gamepad.cpp -o gamepad.exe -static -lws2_32 -lxinput
    goto :done
)

if exist "C:\msys64\mingw64\bin\g++.exe" (
    C:\msys64\mingw64\bin\g++.exe -std=c++17 -O2 -Wall ns-gamepad.cpp -o gamepad.exe -static -lws2_32 -lxinput
    goto :done
)

:done
pause