@rem Rebuilds scope_lens_vs.h / scope_lens_ps.h from scope_lens.hlsl (Windows SDK fxc). The headers are committed.
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x86\fxc.exe"
%FXC% /nologo /O3 /T vs_4_0 /E VSMain /Vn g_scopeLensVs /Fh scope_lens_vs.h scope_lens.hlsl
%FXC% /nologo /O3 /T ps_4_0 /E PSMain /Vn g_scopeLensPs /Fh scope_lens_ps.h scope_lens.hlsl
