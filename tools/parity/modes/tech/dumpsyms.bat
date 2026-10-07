@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
cl /nologo /c /O2 /EHs-c- /GR- /fp:precise %1 >nul
dumpbin /nologo /symbols %~n1.obj | findstr /C:"UNDEF"
