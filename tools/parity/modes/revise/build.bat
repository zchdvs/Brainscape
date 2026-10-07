@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cl /nologo /std:c++17 /O2 /EHsc /fp:precise /I ..\..\..\..\dsp\include /I ..\..\..\..\dsp\src evalmacro.cpp ..\..\..\..\dsp\src\DetMath.cpp /Fe:evalmacro.exe
if exist evalmacro.exe evalmacro.exe
