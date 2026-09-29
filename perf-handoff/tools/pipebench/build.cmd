@echo off
rem Builds pipebench.exe with the emulator's toolchain and its SPIRV-Tools build (clangcl-release).
setlocal
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VS%\VC\Tools\Llvm\x64\bin;%PATH%"
set "SRC=C:\Users\himav\Desktop\kyty ps5-src"
set "BLD=%SRC%\_Build\clangcl-release"
clang-cl /nologo /std:c++20 /O2 /EHsc /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  -I"%SRC%\3rdparty\Vulkan-Headers\include" -I"%SRC%\3rdparty\SPIRV-Tools\include" ^
  "%~dp0pipebench.cpp" /Fe"%~dp0pipebench.exe" /Fo"%~dp0pipebench.obj" -fuse-ld=lld ^
  /link "%BLD%\3rdparty\SPIRV-Tools\source\opt\SPIRV-Tools-opt.lib" ^
  "%BLD%\3rdparty\SPIRV-Tools\source\SPIRV-Tools.lib"
