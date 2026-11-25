#pragma once
// SCOPED EYE (2026-11-14, PROTOTYPE; "the scoped eye can be used for high power magnification when the scope is brought
// close to an eye (and is out of view of the other)"). Engine two-view only (hooks/native_stereo.h).
//
// The gun's sight line is the ZERO (2026-11-23; the optic zero / windage were removed): the shot line exactly as the
// laser casts it, from the weapon profile's Zero right / up beside the hand, along the hand's forward turned by the
// controller aim offset. When one eye is within ScopeEyeBox of that line and up
// to ScopeEyeRelief behind the hand, and the gun has a scope -- its profile's ScopeMagnification (2026-11-17: no longer
// the game's ADS zoom as a fallback; it needs no ADS) -- that eye's view becomes the scope:
//   - its camera moves onto the sight line, level with the eye's own up, looking along the line;
//   - its frustum is the eye's own, shrunk by the magnification about the point where the sight line meets the eye's
//     view (an off-axis frustum: the projection inputs are fovY / aspect / viewport offset, as every eye's), so that point
//     -- the eyepiece's centre -- shows the laser's point (where the shots land; the sight line without the laser) and
//     every angle around it is magnified;
//   - the first-person batches aren't drawn in it (hooks/camera_matrix_test_hook.cpp: they'd be all around the camera);
//   - at Present everything outside the eyepiece (ScopeApparentFov across, narrowing as the eye leaves the line: an exit
//     pupil) is blacked out, and a crosshair drawn while ScopeReticle is on (hooks/present_hook.cpp).
// The other eye keeps its own view, the gun and its scope included. Costs nothing extra: the view was being drawn anyway.

namespace mohw {

struct ScopeEyeView
{
    int eye = -1;           // 0 left, 1 right
    float camera[16] = {};  // world, rows right / up / back / position, as a RenderView's transform
    float fovY = 0.0f, aspect = 1.0f, offX = 0.0f, offY = 0.0f;
};

// Game thread, native_stereo.cpp's MakeEyes, every two-view frame: centre = the head-centre camera the views are built
// from; eyeTan = each eye's frustum tangents {left, right, up, down} as written into its view (FrustumScale included).
// True = an eye is behind the scope: out has its view.
bool ComputeScopedEye(const float centre[16], const float eyeTan[2][4], ScopeEyeView* out);

// Present (game thread): the scoped eye's eyepiece in its half of the frame: centre and radius as fractions of the
// half's width (cx, r) and height (cy). False when no eye is scoped. reticle: draw the crosshair, centred on (rx, ry)
// (fractions of the half): the impact point.
// HOLOGRAPHIC RETICLE (2026-11-17, the user: "Ideally it would function like a holographic optic, wherever you see the
// reticle is the impact point"): the magnified view puts the laser's point at its TRUE direction from that eye -- where
// the SteamVR laser dot overlay is, whatever the eye's offset from the sight line -- and the crosshair goes there. The
// game's own zoom-optic reticle (the "1pzoom" mesh) is no longer drawn in the scoped eye: a mesh on the gun seen from
// the real eye, its offset from the sight line came out multiplied by the magnification.
bool GetScopeMask(int* eye, float* cx, float* cy, float* radiusX, float* radiusY, bool* reticle, float* reticleX,
                  float* reticleY);

// Render threads (hooks/camera_matrix_test_hook.cpp): the eye whose first-person batches aren't drawn (the scoped one),
// or -1.
int GetScopeHiddenEye();

// SCREEN-SPACE LENS PICTURE (2026-11-14, prototype, ScopeLensPicture): each eye that isn't looking through the scope sees
// a magnified picture in the scope's lens (a disc of the gun profile's ScopeLensRadius, centred on the
// zero ScopeLensForward along it), filled from the other eye's half of the frame around the
// laser's point (2026-11-17: where the shots land, as
// that eye sees it; the sight line's vanishing point when the laser isn't running) -- the other eye, 6 cm to the side,
// sees that far field past the scope instead of through it. Present draws it (hooks/present_hook.cpp,
// shaders/scope_lens.hlsl): map = this half's uv -> the side-by-side frame's uv (xy scale, zw offset); circle = the
// lens's centre (uv), radius (in uv x) and its height-to-width ratio in uv; reticle = the impact point in this half (uv),
// unused, unused, 1 = draw the crosshair.
// HOLOGRAPHIC (2026-11-17): the picture is anchored on the laser's point at its true direction from this eye, not on
// the lens's centre -- the lens is only the window it's seen through -- so the impact point and its crosshair stay on
// the target (and on the SteamVR laser dot) wherever the eye is; they slide inside the lens as the eye moves.
struct ScopeLensPicture
{
    bool on = false;
    float map[4] = {};
    float circle[4] = {};
    float reticle[4] = {};
};
// Present (game thread): both eyes' pictures (on = false for an eye with none). False when there are none at all.
bool GetScopeLensPictures(ScopeLensPicture out[2]);

// RED DOT SIGHT (2026-11-22, the user: "an alternate that is purely a red dot holographic with no magnification? Then we
// could hide the laser and have a more natural aiming experience that's perfectly zeroed"). A gun whose profile's Sight
// is "Red dot" gets no scoped eye and no lens picture: each eye sees a red dot (RedDotSize across) where IT sees the
// impact point -- the laser's point at its true direction from that eye, the sight line's vanishing point without the
// laser -- drawn only inside the red dot's own window (2026-11-23: a disc of the profile's RedDotWindowRadius, centred on
// the zero RedDotWindowForward along it) as that eye sees it. Zeroed at whatever range the laser hits, with no parallax.
// Passed to Present as ScopeLensPictures with reticle = {x, y, dot radius in uv x, 2}. With "Scope shows: only with
// the game's magnifier on" and a magnification, the game's magnifier turns it into the scope. Engine two-view only, as
// the scope.

} // namespace mohw
