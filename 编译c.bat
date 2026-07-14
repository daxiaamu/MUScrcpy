@echo off
windres start_scrcpy_c.rc -O coff -o start_scrcpy_c.res
if errorlevel 1 goto error
gcc start_scrcpy_c.c start_scrcpy_c.res -municode -mwindows -O2 -s -o MUScrcpy.exe -lcomctl32 -lgdi32 -ladvapi32 -lshell32
if errorlevel 1 goto error
echo Build completed.
exit /b 0
:error
echo Build failed.
exit /b 1
