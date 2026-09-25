#pragma once

// Cross-process, cross-bitness contract between the 32-bit injected mod
// (proxy_dll, eventually) and the 64-bit VR companion process
// (mohwvr_companion.exe).
//
// IDXGIResource1::CreateSharedHandle / ID3D11Device1::OpenSharedResourceByName
// (the "named shared resource" convenience API added in D3D11.1) turned out
// not to work on this machine -- confirmed via a same-process self-test:
// CreateTexture2D and CreateSharedHandle both succeeded, but
// OpenSharedResourceByName on the exact same texture, in the exact same
// process, still failed with E_INVALIDARG. That rules out cross-process,
// cross-bitness, session, and privilege-level causes entirely (the earlier
// "backslash in the name" theory was also wrong -- a name with no backslash
// at all failed identically). Most likely this driver/OS combination simply
// doesn't implement the named-lookup path properly, which is a known spotty
// area outside pure first-party driver/OS combos.
//
// So instead: each eye is still a D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX +
// D3D11_RESOURCE_MISC_SHARED_NTHANDLE texture (keyed-mutex sync requires
// NTHANDLE), but handed off via an explicit HANDLE duplication instead of a
// name lookup -- the much older, universally-supported mechanism: the
// producer publishes its PID and its own local (unnamed) shared handles
// through a plain Win32 file-mapping (ordinary shared memory, unrelated to
// DXGI's own naming system and not observed to have the same problem); the
// consumer opens that mapping, calls OpenProcess(PROCESS_DUP_HANDLE, ...)
// on the published PID, then DuplicateHandle()s each raw handle into its
// own process before calling the ORIGINAL (non-"ByName") ID3D11Device::
// OpenSharedResource(HANDLE, ...), which is core D3D11 functionality that
// has existed since D3D10.
//
// Handoff uses the standard D3D11 keyed-mutex ping-pong pattern: whichever
// side wants to write a new frame does AcquireSync(kFreeKey), writes, then
// ReleaseSync(kReadyKey); whichever side wants to read acquires
// kReadyKey, reads, then releases back to kFreeKey. The very first
// AcquireSync call against a freshly created keyed-mutex resource must use
// key 0, which is why kFreeKey is 0.
//
// (Producer and consumer are still expected to run in the same interactive
// Windows session for unrelated reasons -- see docs/phase2d_findings.md.)

#include <windows.h>
#include <dxgiformat.h>

namespace mohwvr::ipc {

// Plain Win32 shared memory (CreateFileMappingW/OpenFileMappingW), not a
// DXGI-named object -- deliberately a completely different OS subsystem
// from the one that failed above.
constexpr wchar_t kHandleExchangeMapName[] = L"MOHWVR_HandleExchange";

struct HandleExchangeBlock
{
    LONG ready; // 0 until the producer has published valid handles
    DWORD producerPid;
    UINT64 leftHandle;  // raw HANDLE value, valid only within producerPid until duplicated
    UINT64 rightHandle;
    // Bumped by the producer every time it (re)creates that eye's shared texture. The raw HANDLE values above are NOT
    // enough to detect a recreate: after CloseHandle/CreateSharedHandle the OS can hand back the SAME numeric handle, so
    // the companion kept reading the orphaned old texture (2026-09-20: right eye frozen / flashing after a startup
    // ResizeBuffers).
    UINT64 leftGeneration;
    UINT64 rightGeneration;
    // UI extraction (2026-09-21): a THIRD pair, alternating the same way as left/right, holding the "world, no UI yet"
    // snapshot draw_trace_diag.cpp captures each frame (see project memory: project_mohw_eye_matched_display_method.md).
    // The companion diffs this against the normal (world+UI) eye texture to isolate the UI as its own layer. Same
    // handle-reuse caveat as leftGeneration/rightGeneration above -- always check the generation, not just the handle.
    UINT64 uiLeftHandle;
    UINT64 uiRightHandle;
    UINT64 uiLeftGeneration;
    UINT64 uiRightGeneration;
};

constexpr UINT64 kFreeKey = 0;
constexpr UINT64 kReadyKey = 1;

// Matches this SteamVR/OpenXR runtime's negotiated swapchain format
// (confirmed live: xrEnumerateSwapchainFormats offers no UNORM variant on
// this system, only UNORM_SRGB). Both sides must agree on this so
// CopySubresourceRegion works without a format-converting shader -- a
// simplification worth revisiting once eye textures come from the game's
// actual backbuffer format instead of a synthetic test pattern.
constexpr DXGI_FORMAT kSharedEyeFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

// Head pose, companion -> proxy_dll -- the first channel flowing in the
// opposite direction from everything above (which is all proxy_dll ->
// companion). The companion already computes real per-eye pose every frame
// via xrLocateViews; this publishes it back so the game side can eventually
// drive the camera with it (see docs/companion_process_findings.md's task
// list: "Patch per-eye view matrix with HMD pose + IPD offset").
//
// No keyed mutex here, deliberately -- unlike the eye textures (where a
// half-written frame would be a visibly corrupt image), a torn read of a
// few floats mid-update is at worst one frame of imperceptible orientation
// jitter, not a correctness problem. This is the same "continuous data
// doesn't need frame-exact sync" reasoning the motion-controls research
// (docs/motion_controls_research.md) found bioshock-vr's own Gamepad state
// channel uses: no locks, just publish-and-overwrite.
//
// Orientation only for now (a plain quaternion, XR LOCAL reference space,
// eye 0's pose since both eyes share the same head rotation) -- position
// is included for completeness but the original plan deliberately scoped
// the actual camera integration to ROTATION only, layered on top of the
// game's own view rather than replacing its position, so as not to fight
// the game's own movement/collision logic. A recenter mechanism (capturing
// a "zero" orientation to diff against) is intentionally not part of this
// wire format -- that's a policy decision for whichever side applies the
// pose, not something the channel itself needs to know about.
constexpr wchar_t kHeadPoseMapName[] = L"MOHWVR_HeadPose";

struct HeadPoseBlock
{
    LONG ready;           // 0 until the companion has published at least one real pose
    UINT64 frameCounter;  // increments on every publish; consumers that care about staleness can compare against their own last-seen value
    float orientationX, orientationY, orientationZ, orientationW; // quaternion, XR LOCAL space
    float positionX, positionY, positionZ;                        // meters, XR LOCAL space
};

// Right motion controller's aim pose, companion -> proxy_dll -- same
// transport pattern and same "no lock, publish-and-overwrite" reasoning as
// HeadPoseBlock above. Sourced from the OpenXR "aim" pose (not "grip") on
// /user/hand/right, since aim is the pose meant for pointing/targeting
// (grip is oriented for how the controller sits in the hand, which differs
// by controller model). Orientation only -- position is included for
// completeness but the first consumer (hooks/fire_candidate_redirect_hook.cpp,
// hooks/bullet_raycast_redirect_hook.cpp) only uses orientation, per the
// user's explicit direction to redirect the shot's ROTATION from the
// controller, not its position (the game's own muzzle/reticle origin is
// left as-is).
constexpr wchar_t kRightControllerPoseMapName[] = L"MOHWVR_RightControllerPose";

struct ControllerPoseBlock
{
    LONG ready;          // 0 until the companion has published at least one real pose
    UINT64 frameCounter; // increments on every publish
    float orientationX, orientationY, orientationZ, orientationW; // quaternion, XR LOCAL space
    float positionX, positionY, positionZ;                        // meters, XR LOCAL space
    // Analog trigger value on /user/hand/right/input/trigger/value, 0
    // (released) to 1 (fully pulled). Added so the trigger can stand in for
    // a left mouse click (task: "make the controller trigger act as a left
    // mouse input for the time being") without a separate IPC channel --
    // it's cheap to publish alongside the pose every frame regardless of
    // whether a consumer is using it yet.
    float triggerValue;
};

// Headset view geometry, companion -> proxy_dll (2026-09-20). Published every frame
// from xrLocateViews so the game side can derive its render FOV/aspect from the REAL
// per-eye frustum instead of the FovScale/cover-crop workarounds. Angles are the OpenXR
// XrFovf half-angles in radians (angleLeft/angleDown are negative), eye 0 = left, 1 = right.
constexpr wchar_t kHmdViewMapName[] = L"MOHWVR_HmdView";

struct HmdViewBlock
{
    LONG ready;          // 0 until the companion has published at least once
    UINT64 frameCounter; // increments on every publish
    float angleLeft[2];
    float angleRight[2];
    float angleUp[2];
    float angleDown[2];
    float ipdMeters;     // distance between the two located eye positions
    LONG swapchainWidth; // per-eye swapchain size actually created (= runtime recommended size)
    LONG swapchainHeight;
    LONG viewMode;       // companion view mode (see kViewModeInnerEdge); the game uses it to pick how it sets the world FOV
    float aspectTrim;    // inner-edge aspect trim (companion keys Up/Down); the game divides its projection aspect by this in inner-edge mode
    float sourceAspect;  // width/height of the game frame the companion is sampling (0 = unknown)
    float innerStrip;    // inner-edge crop strip (fraction of frame width dropped per eye); the game uses it to compute an undistorted vertical FOV
    // Manual camera selector for the true-frustum view mode (2026-09-21): several unrelated camera objects can share the
    // exact fov value mode 3 targets (shadows, cube faces, ...); -1 = write to every match (default, matches pre-selector
    // behavior), >=0 = only write to the Nth DISTINCT object seen matching this mode-3 session (mod the live count, so
    // any value works regardless of how many have been discovered). ',' '.' in companion/main.cpp cycle it.
    LONG frustumCandidateIndex;
};

// HmdViewBlock::viewMode value for the inner-edge crop mode (companion/main.cpp cycles modes with Insert).
constexpr LONG kViewModeInnerEdge = 2;
// Companion view mode 3: the game renders the eye's exact asymmetric frustum (hooks/eye_matched_fov.cpp + projection_aspect_hook.cpp).
constexpr LONG kViewModeTrueFrustum = 3;

// Render-pose stamp (game -> companion, 2026-09-21, EXPERIMENTAL): for each eye, the head orientation (XR local space) that the
// most recent game frame routed to that eye was actually RENDERED for, reconstructed from the camera rotation the game used
// (30 Hz sim tick + smoothing + pair freeze). The companion submits that frame with this orientation so the compositor can
// reproject the difference to the current head pose, instead of being told the (older) image matches the newest pose.
constexpr wchar_t kRenderPoseMapName[] = L"MOHWVR_RenderPose";

struct RenderPoseBlock
{
    LONG ready;            // 0 until the game has published at least once
    UINT64 counter[2];     // per eye (0 = left, 1 = right); increments each time that eye gets a new frame + stamp
    float quat[2][4];      // per eye, x y z w
    float pos[2][3];       // per eye: HEAD position (XR local space, meters) the frame was rendered for; only meaningful if posValid
    LONG posValid[2];      // 1 if the game applied head-position tracking for that frame (so pos is the position it rendered from)
};

} // namespace mohwvr::ipc
