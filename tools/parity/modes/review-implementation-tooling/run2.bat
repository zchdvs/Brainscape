@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
cl /nologo /std:c++17 /O2 /fp:precise /I ..\..\..\..\dsp\include /c comdat_probe.cpp >nul
echo === definitions naming brainscape:: in a bsc object
dumpbin /nologo /symbols comdat_probe.obj | findstr /C:"brainscape@@" | findstr /V /C:"UNDEF"
echo === COMDAT section headers
dumpbin /nologo /headers comdat_probe.obj | findstr /C:"COMDAT"
