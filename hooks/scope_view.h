#pragma once
// SCOPED EYE (2026-11-14, PROTOTYPE; "the scoped eye can be used for high power magnification when the scope is brought
// close to an eye (and is out of view of the other)"). Engine two-view only (hooks/native_stereo.h).
//
// The gun's sight line is the weapon profile's optic zero: from the weapon hand, OpticHeight up and OpticWindageOffset
// right, along the hand's forward (the line the optic dot sits on). When one eye is within ScopeEyeBox of that line and up
// to ScopeEyeRelief behind the hand, and the gun has a scope -- its own ScopeMagnification, or the game's ADS zoom (the
// view's FOV against the hip FOV) of at least ScopeMinZoom while ADS is held -- that eye's view becomes the scope:
//   - its camera moves onto the sight line, level with the eye's own up, looking along the line;
//   - its frustum is the eye's own, shrunk by the magnification about the point where the sight line meets the eye's
//     view (an off-axis frustum: the projection inputs are fovY / aspect / viewport offset, as every eye's), so that point
//     shows the sight line and every angle around it is magnified;
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
// from; eyeTan = each eye's frustum tangents {left, right, up, down} as written into its view (FrustumScale included);
// gameFovY = the game's own vertical FOV for this frame (view 0's before the eyes are written), radians. True = an eye
// is behind the scope: out has its view.
bool ComputeScopedEye(const float centre[16], const float eyeTan[2][4], float gameFovY, ScopeEyeView* out);

// Present (game thread): the scoped eye's eyepiece in its half of the frame: centre and radius as fractions of the
// half's width (cx, r) and height (cy). False when no eye is scoped. reticle: draw the crosshair.
bool GetScopeMask(int* eye, float* cx, float* cy, float* radiusX, float* radiusY, bool* reticle);

// Render threads (hooks/camera_matrix_test_hook.cpp): the eye whose first-person batches aren't drawn (the scoped one),
// or -1.
int GetScopeHiddenEye();
// Render threads: the scoped eye is drawing the game's own zoom optic (the "1pzoom" mesh, its reticle): the drawn
// crosshair stands aside while it does.
void NoteScopeGameReticle();

} // namespace mohw
