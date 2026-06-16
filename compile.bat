@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cl /EHsc fixed_mytry2.cpp /link Gdiplus.lib Ws2_32.lib Gdi32.lib User32.lib Ole32.lib
