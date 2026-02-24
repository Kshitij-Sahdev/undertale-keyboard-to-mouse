@echo off
g++ -O2 -std=c++20 -mwindows -o MouseJoystick.exe MouseJoystick.cpp ^
    -luser32 -lgdi32 -lgdiplus -lpsapi -ldwmapi
echo Done — MouseJoystick.exe ready.
pause