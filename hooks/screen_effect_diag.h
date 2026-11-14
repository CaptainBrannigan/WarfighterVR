#pragma once
// THE SCREEN EFFECTS WITH TWO VIEWS (2026-11-09: "the dust effect was still only in the right eye ... a UI effect, not
// a 3D particle effect"). Diagnostic first, now also the fix (ScreenEffectsBothEyes). The game's screen effects
// (ScreenEffectComponent: full-frame or frame-part textures over the view, DustAccumulation among their settings) are
// drawn per view by FUN_00FD2FA0, called from the per-view render function FUN_010130C0 (in a loop over the views on
// one thread, FUN_0101F5D0). Its inputs:
//  - the view (the per-view block +0x10, a ViewDesc copy) and its viewport (+0x30, x / y / w / h as uint16);
//  - the view's visual-environment state (block +0x1AD30, 0xAF0 bytes), whose +0xA20 / +0xA24 are the effect's two
//    ref-counted resources: the call is skipped unless one is set; +0xA24 goes through FUN_00FD2030 first.
// Every view's state is copied from the one global state each frame (FUN_0101CFA0: the manager DAT_02B183A0's +0xB20,
// FUN_00FDA9E0, its sub-copy FUN_00FD4DD0 AddRefs both resources), so both views have the effect. The draw list is
// then kicked as its own pass into whatever target the render interface (DAT_02AC16F4, vtable +0x04) has bound
// (FUN_00779D40). The log showed both views drawing it every frame into the same target, so the left half's is lost
// to something later in the right view's render: with the setting on, the left view's call is held and made right
// after the right view's (see HeldCall in the .cpp). Logs, while an effect is active, the calls (which half, the
// resources, the bound target) and every 10 s how many left calls were held, made late and dropped, to
// mohwvr_screeneffect.log.

namespace mohw {

bool InstallScreenEffectDiag();
void RemoveScreenEffectDiag();

} // namespace mohw
