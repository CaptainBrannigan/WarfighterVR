#pragma once
// SteamVR Input (IVRInput_011) for the motion controllers, called through raw vtables like the rest of
// openvr_direct (see openvr_direct.h's top comment for why openvr_api.lib is never linked). The action manifest and
// default bindings are embedded in vr_input.cpp and written next to the DLL at connect time, so players rebind in
// SteamVR's own binding UI rather than in our ini.

namespace mohw::openvr_direct {

// Buttons/sticks. Poses are published separately, per hand, via hooks/companion_bridge.h's controller overrides.
struct VrActionState
{
    bool fire = false;
    float moveX = 0.0f, moveY = 0.0f; // left stick by default, -1..1, +Y = pushed forward
    float turnX = 0.0f;               // right stick X by default, -1..1, + = pushed right
};

// Connect thread, once, before SubmitThreadProc starts. input = the IVRInput_011 interface pointer,
// trackingUniverse = the compositor's own tracking space (so controller poses share the head pose's space).
// Returns false if registration failed; everything else keeps working without controllers.
bool InitVrInput(void* input, int trackingUniverse);

// Submit thread, once per loop iteration, right after WaitGetPoses (pose actions return that call's prediction).
void UpdateVrInput();

// Game thread. False until SteamVR Input has produced its first update.
bool GetVrActionState(VrActionState* out);

// Lock-free read of just the stick axes, for hooks/xinput_hook.cpp, which runs inside the game's own controller
// polling. Same values and false-until-first-update behaviour as GetVrActionState.
bool GetVrSticks(float* moveX, float* moveY, float* turnX);

} // namespace mohw::openvr_direct
