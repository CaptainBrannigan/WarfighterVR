#pragma once
// Direct hook on a raw internal engine function (not a D3D11 API entry
// point) -- the first hook of this kind in the project, and the goal of
// branching off into Ghidra in the first place (task: the DX11 constant-
// buffer patching workflow needed a lot of iteration to get stable; find
// a sturdier hook point in the engine's own code instead).
//
// FUN_007738c0 (sdk/mohw_offsets.h: OFFSET_UPDATEVIEWCONSTANTS), found via
// Ghidra analysis of a live memory dump, is the function that uploads the
// "m_viewConstants" camera buffer -- confirmed via a literal debug string
// in its own decompiled code that independently matches the "viewConstants"
// HLSL cbuffer name already found via shader disassembly (see
// docs/stereo_view_matrix_investigation.md).
//
// This first pass is diagnostic only: hooks the function and logs its
// param_1 (the "this"-like object pointer, passed in ECX per __thiscall)
// alongside GameRenderer::Singleton(), to determine the exact relationship
// between them and confirm/refute that param_1 is (or points into)
// GameRenderViewParams, per sdk/renderview.h.

namespace mohw {

bool InstallEngineFunctionHook();
void RemoveEngineFunctionHook();

} // namespace mohw
