@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cl /nologo /std:c++17 /O2 /EHsc /fp:precise /c audit_probe_fp.cpp audit_probe_int.cpp >nul
echo === FP object: external UNDEF symbols
dumpbin /nologo /symbols audit_probe_fp.obj | findstr /C:"UNDEF" | findstr /C:"External"
echo === INT object: external UNDEF symbols
dumpbin /nologo /symbols audit_probe_int.obj | findstr /C:"UNDEF" | findstr /C:"External"
