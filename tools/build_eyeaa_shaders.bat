@echo off
rem EYEAA: compile src\shaders\eyeaa.hlsl into byte-array headers (src\shaders\eyeaa_*.h). Run after editing the shader.
setlocal
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SRC=%~dp0..\src\shaders\eyeaa.hlsl
set OUT=%~dp0..\src\shaders
%FXC% /nologo /O3 /T vs_5_0 /E VS_Edge  /Vn g_eyeaa_vs_edge  /Fh "%OUT%\eyeaa_vs_edge.h"  "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_Edge  /Vn g_eyeaa_ps_edge  /Fh "%OUT%\eyeaa_ps_edge.h"  "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T vs_5_0 /E VS_Blend /Vn g_eyeaa_vs_blend /Fh "%OUT%\eyeaa_vs_blend.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_Blend /Vn g_eyeaa_ps_blend /Fh "%OUT%\eyeaa_ps_blend.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T vs_5_0 /E VS_Neigh /Vn g_eyeaa_vs_neigh /Fh "%OUT%\eyeaa_vs_neigh.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_Neigh /Vn g_eyeaa_ps_neigh /Fh "%OUT%\eyeaa_ps_neigh.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T vs_5_0 /E VS_Full  /Vn g_eyeaa_vs_full  /Fh "%OUT%\eyeaa_vs_full.h"  "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_Soft  /Vn g_eyeaa_ps_soft  /Fh "%OUT%\eyeaa_ps_soft.h"  "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_Copy  /Vn g_eyeaa_ps_copy  /Fh "%OUT%\eyeaa_ps_copy.h"  "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_GlowPre /Vn g_eyeaa_ps_glowpre /Fh "%OUT%\eyeaa_ps_glowpre.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_GlowDown /Vn g_eyeaa_ps_glowdown /Fh "%OUT%\eyeaa_ps_glowdown.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_GlowUp /Vn g_eyeaa_ps_glowup /Fh "%OUT%\eyeaa_ps_glowup.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_LumLog /Vn g_eyeaa_ps_lumlog /Fh "%OUT%\eyeaa_ps_lumlog.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_Adapt /Vn g_eyeaa_ps_adapt /Fh "%OUT%\eyeaa_ps_adapt.h" "%SRC%" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E main /Vn g_tonefix_ps /Fh "%OUT%\tonefix_ps.h" "%~dp0..\src\shaders\tonefix.hlsl" || exit /b 1
%FXC% /nologo /O3 /T vs_5_0 /E VS_Full /Vn g_toneglow_vs /Fh "%OUT%\toneglow_vs.h" "%~dp0..\src\shaders\toneglow.hlsl" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_HPre /Vn g_toneglow_pre /Fh "%OUT%\toneglow_pre.h" "%~dp0..\src\shaders\toneglow.hlsl" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_HDown /Vn g_toneglow_down /Fh "%OUT%\toneglow_down.h" "%~dp0..\src\shaders\toneglow.hlsl" || exit /b 1
%FXC% /nologo /O3 /T ps_5_0 /E PS_HUp /Vn g_toneglow_up /Fh "%OUT%\toneglow_up.h" "%~dp0..\src\shaders\toneglow.hlsl" || exit /b 1
echo EYEAA shaders built.
