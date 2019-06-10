@echo off

set commonCompilerFlags= -MD -nologo -Gm- -GR- -EHa- -O2 -Oi -arch:AVX -fp:fast -WX -W4 -WL -wd4100 -wd4201 -wd4189 -wd4505 -wd4127 -wd4238 -wd4324 -we4746 -FC -Z7 -Fm -D_DEBUG -DROOTD12INCLUDE
set commonLinkerFlags= -incremental:no -opt:ref user32.lib Gdi32.lib winmm.lib d3d12.lib dxgi.lib d3dcompiler.lib

REM \WL one-line diagnostic
REM \arch:AVX: for AVX support
REM -Zo: better debug info in optimized code
REM -fp:fast for floating point
REM subsystem:console or windows usually, 5.2 for windows xp, 5.1 for windows xp 32 bit
REM -MT statically link c runtime lib into exe
REM -Gm: enables minimal rebuild
REM -Fm creates map file
REM opt:ref: tell the linker don't put symbols to the executable if they are not used

IF NOT EXIST build mkdir build
pushd build
del *.pdb

REM fxc /nologo /Zi /DSIMPLE_SHADER_VS /E simpleShaderVS /T vs_5_1 /Fo simpleShaderVS.cso ..\code\shaders.hlsl
fxc /nologo /Zi /DHEIGHT_MAPPING_SHADER_VS /E heightMappingShaderVS /T vs_5_1 /Fo heightMappingShaderVS.cso ..\code\shaders.hlsl

REM fxc /nologo /Zi /DPBR_SHADER_PS /E pbrShaderPS /T ps_5_1 /Fo pbrShaderPS.cso ..\code\shaders.hlsl
fxc /nologo /Zi /DPBR_NORMAL_CALC_SHADER_PS /E pbrNormalCalcShaderPS /T ps_5_1 /Fo pbrNormalCalcShaderPS.cso ..\code\shaders.hlsl
REM fxc /nologo /Zi /DFRACTAL_SHADER_PS /E fractalShaderPS /T ps_5_1 /Fo fractalShaderPS.cso ..\code\shaders.hlsl

REM fxc /nologo /Zi /DTESS_SHADER /DTESS_SHADER_VS /E tessShaderVS /T vs_5_1 /Fo tessShaderVS.cso ..\code\shaders.hlsl
REM fxc /nologo /Zi /DTESS_SHADER /DTESS_SHADER_HS /E tessShaderHS /T hs_5_1 /Fo tessShaderHS.cso ..\code\shaders.hlsl
REM fxc /nologo /Zi /DTESS_SHADER /DTESS_SHADER_DS /E tessShaderDS /T ds_5_1 /Fo tessShaderDS.cso ..\code\shaders.hlsl


cl %commonCompilerFlags% ..\code\main.cpp   /link %commonLinkerFlags%

popd

echo DONE!