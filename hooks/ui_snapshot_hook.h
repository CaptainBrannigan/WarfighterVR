#pragma once
// SUPERSEDED before use (2026-09-21): the UI-snapshot capture this file was meant to hold instead lives in
// hooks/draw_trace_diag.cpp (its Hook_OM/Hook_Draw*/DrawTraceNoteDraw already own the only permanent hooks on
// OMSetRenderTargets and the draw calls -- MinHook allows one hook per target, so a second file hooking the same
// D3D11 vtable slots would fail to install). See draw_trace_diag.h for the real capture (GetUiSnapshotSource) and
// project memory project_mohw_eye_matched_display_method.md for the plan this is part of. Not compiled into the
// project; kept only so a later reader doesn't wonder where this name went.
