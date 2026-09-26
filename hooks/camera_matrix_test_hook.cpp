#include "camera_matrix_test_hook.h"

#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/mohw_common.h"
#include "../sdk/vr_math.h"
#include "companion_bridge.h"

#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <cstdint>
#include <atomic>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_cameramatrix.log";
constexpr float kYawDegrees = 0.0f; // yaw test retired -- now a translation test (offsets below)

// Translation test (2026-09-19): objects move by right*row0 + up*row1 + fwd*(-row2)
// of the camera matrix passed to the setter; the camera position moves the opposite way
// (view = inverse of camera). Units assumed metres. The hotkeys that stepped these are
// retired (see PollHotkeys); defaults are 0 so the weapon sits where the game puts it --
// the 0.5/0.5 test offset used to verify this hook was still being applied (2026-09-26).
constexpr float kDefaultRight = 0.0f;
constexpr float kDefaultUp = 0.0f;
constexpr float kDefaultFwd = 0.0f;
std::atomic<float> g_offRight{kDefaultRight};
std::atomic<float> g_offUp{kDefaultUp};
std::atomic<float> g_offFwd{kDefaultFwd};
constexpr int kGroupSize = 12;      // first-person group: 12 batches per view (cnt4, 9 singles, cnt3)
constexpr int kSelectAll = -1;

using CameraMatrixSetFn = void(__thiscall*)(void* thisPtr, const float* matrix);

// "Player skeleton loaded" signal (2026-09-22) -- see IsPlayerSkeletonLoaded's own comment in the header.
std::atomic<DWORD> g_lastViewmodelMatrixTick{0};

CameraMatrixSetFn g_original = nullptr;
void* g_hookAddress = nullptr;
std::atomic<int> g_patched{0};
std::atomic<int> g_selected{0};
std::atomic<int> g_dumpRemaining{0};
std::atomic<int> g_lastLoggedSelected{-2};
std::atomic<int> g_seqLogLines{0};

// FUN_008B2AB0 hands the 12 first-person batches to this call site in a fixed
// order per view: the count==4 batch first, then 9 singles, then the count==3
// batch. Per-thread because different views are built on different threads.
thread_local uintptr_t t_lastBatch = 0;
thread_local int t_seq = -1;
thread_local uint32_t t_lastCount = 0;

// 0 = use the index selector; 1 = weapon batches only (count != 3);
// 2 = body+hands batch only (count == 3). Confirmed live 2026-09-19: the
// count==3 batch is body+hands for both rifle and pistol groups.
std::atomic<int> g_roleMode{1}; // default: weapon only

// Instance-hide test for the count==3 (body+hands) batch: -1 = off, 0..3 = do not
// submit that instance index. Keys: ']' next, '[' previous.
std::atomic<int> g_hideInstance{-1};
std::atomic<int> g_lastLoggedHide{-2};
std::atomic<bool> g_hideBodyAll{false}; // '\' toggles: hide every instance of the count==3 (legs+arms+hands) batch

using DrawItemSubmitFn = void(__cdecl*)(void* item, void* drawState);
DrawItemSubmitFn g_originalSubmit = nullptr;
void* g_submitAddress = nullptr;

// ---- Controller drive (v2, 2026-09-20) ------------------------------------------
// v1 expressed the controller in the head's rotating frame, so head look moved the
// weapon. v2 uses the ABSOLUTE controller pose in XR space and cancels the game
// camera's own rotation since calibration, so the weapon is independent of head look.
//   Calibrate (numpad 0, hold the controller where the gun sits): captures the head
//   and controller poses (XR) and the camera basis B0 of the first patched call.
//   Alignment A (rows = world images of the XR basis vectors) = (S o (conj(hq0) e_i)) * B0.
//   Per call: relXR = ctrlP - headP (XR, unrotated); Rc_w = A^T Rx A (Rx = row-form of
//   ctrlQ*conj(c0)); Rh_w = B0^T B(t) (camera rotation since cal); Rr = Rh_w^T Rc_w;
//   pivot = pos + posHead_cal*B(t) (weapon's current camera-attached rest position);
//   P_des = pos + relXR*A; d = P_des - pivot.
//   T(p) = (p-pivot)*Rr + pivot + d  =>  B' = B*Rr^T, pos' = pivot - (pivot+d-pos)*Rr^T.
//   At calibration Rr = I and d = 0 (weapon unchanged), matching the confirmed live
//   translation test when only d is nonzero. S = per-axis sign flips (numpad 7/9/'.'),
//   XR head-local (x right, y up, z back) assumed to match camera rows (right/up/back).
std::atomic<bool> g_controllerMode{false};
std::atomic<bool> g_calRequested{false};
std::atomic<bool> g_haveCal{false};
std::atomic<int> g_signX{1};
std::atomic<int> g_signY{1};
std::atomic<int> g_signZ{1};

struct Calib
{
    Quat hq0;
    Quat c0;
    Vec3 relXR0;
    float B0[3][3];
};
Calib g_cal{};

bool ReadPoses(Quat* hq, Vec3* headP, Quat* cq, Vec3* ctrlP)
{
    mohwvr::ipc::HeadPoseBlock hp{};
    mohwvr::ipc::ControllerPoseBlock cp{};
    if (!GetHeadPose(&hp) || !GetRightControllerPose(&cp))
        return false;
    *hq = Quat{hp.orientationX, hp.orientationY, hp.orientationZ, hp.orientationW};
    *headP = Vec3{hp.positionX, hp.positionY, hp.positionZ};
    *cq = Quat{cp.orientationX, cp.orientationY, cp.orientationZ, cp.orientationW};
    *ctrlP = Vec3{cp.positionX, cp.positionY, cp.positionZ};
    return true;
}

// Row-vector 3x3 rotation matrix (v * Rl) equivalent to the quaternion.
void QuatToRowMatrix(Quat q, float Rl[3][3])
{
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-6f)
        q = Quat{0, 0, 0, 1};
    else
        q = Quat{q.x / n, q.y / n, q.z / n, q.w / n};
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    float Rc[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                      {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                      {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            Rl[i][j] = Rc[j][i];
}

void RowMulVec(const float v[3], const float M[3][3], float out[3])
{
    for (int j = 0; j < 3; ++j)
        out[j] = v[0] * M[0][j] + v[1] * M[1][j] + v[2] * M[2][j];
}

void MatMul3(const float X[3][3], const float Y[3][3], float out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = X[i][0] * Y[0][j] + X[i][1] * Y[1][j] + X[i][2] * Y[2][j];
}

void Transpose3(const float X[3][3], float out[3][3])
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i][j] = X[j][i];
}

std::atomic<bool> g_autoSigns{true};

// Angle (degrees) between the camera rotation the game actually applied since
// calibration (Rh_w = B0^T B) and the rotation predicted from the real head's rotation
// (q_h = hq * conj(hq0)) under the given axis-sign mapping S. ~0 when S is right.
float HeadConsistencyErrorDeg(const float S[3], const Quat& hq0, const Quat& hq, const float B0[3][3], const float B[3][3])
{
    float A[3][3];
    Quat hqInv0 = QuatConjugate(hq0);
    for (int i = 0; i < 3; ++i)
    {
        Vec3 e{i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
        Vec3 h = QuatRotateVector(hqInv0, e);
        float hl[3] = {S[0] * h.x, S[1] * h.y, S[2] * h.z};
        for (int k = 0; k < 3; ++k)
            A[i][k] = hl[0] * B0[0][k] + hl[1] * B0[1][k] + hl[2] * B0[2][k];
    }
    float AT[3][3], Rx[3][3], T1[3][3], Rpred[3][3], B0T[3][3], Ract[3][3], RpredT[3][3], E[3][3];
    Transpose3(A, AT);
    QuatToRowMatrix(QuatMultiply(hq, QuatConjugate(hq0)), Rx);
    MatMul3(AT, Rx, T1);
    MatMul3(T1, A, Rpred);
    Transpose3(B0, B0T);
    MatMul3(B0T, B, Ract);
    Transpose3(Rpred, RpredT);
    MatMul3(RpredT, Ract, E);
    float c = (E[0][0] + E[1][1] + E[2][2] - 1.0f) * 0.5f;
    return acosf(fmaxf(-1.0f, fminf(1.0f, c))) * (180.0f / kPi);
}

bool ApplyControllerDrive(float m[16], float manualRight, float manualUp, float manualFwd, const unsigned char* batch,
                          uint32_t count)
{
    float B[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};

    // Rigid-attachment check: does the weapon's offset/orientation RELATIVE TO THE CAMERA
    // stay constant as the head turns? (Assumption the head-cancel math relies on.)
    if (batch && count == 1)
    {
        const float* bt = reinterpret_cast<const float*>(batch + 0x40);
        if (bt[0] != 0.0f || bt[1] != 0.0f || bt[2] != 0.0f)
        {
            static std::atomic<DWORD> lastRigid{0};
            DWORD tk = GetTickCount();
            DWORD pv = lastRigid.load();
            if (tk - pv > 500 && lastRigid.compare_exchange_strong(pv, tk))
            {
                const float* bm = reinterpret_cast<const float*>(batch + 0x10);
                float O[3][3] = {{bm[0], bm[1], bm[2]}, {bm[4], bm[5], bm[6]}, {bm[8], bm[9], bm[10]}};
                float BTm[3][3], Q[3][3];
                Transpose3(B, BTm);
                MatMul3(BTm, O, Q);
                float qang = acosf(fmaxf(-1.0f, fminf(1.0f, (Q[0][0] + Q[1][1] + Q[2][2] - 1.0f) * 0.5f))) * (180.0f / kPi);
                float dv[3] = {bt[0] - m[12], bt[1] - m[13], bt[2] - m[14]};
                float loc[3];
                for (int i = 0; i < 3; ++i)
                    loc[i] = dv[0] * B[i][0] + dv[1] * B[i][1] + dv[2] * B[i][2];
                float camRot = -1.0f;
                if (g_haveCal.load())
                {
                    float B0T[3][3], Rh[3][3];
                    Transpose3(g_cal.B0, B0T);
                    MatMul3(B0T, B, Rh);
                    camRot = acosf(fmaxf(-1.0f, fminf(1.0f, (Rh[0][0] + Rh[1][1] + Rh[2][2] - 1.0f) * 0.5f))) * (180.0f / kPi);
                }
                MOHW_LOG(kLogFile,
                           "RIGID CHECK (count==1 weapon part): camRotSinceCal=%.1f deg | offset in camera-local (right,up,back)=(%.3f,%.3f,%.3f) | batch-rotation-vs-camera=%.1f deg  (both should stay constant if the weapon is rigidly camera-attached)",
                           camRot, loc[0], loc[1], loc[2], qang);
            }
        }
    }

    Quat hq{}, cq{};
    Vec3 headP{}, ctrlP{};
    if (!ReadPoses(&hq, &headP, &cq, &ctrlP))
        return false;
    Vec3 relXR{ctrlP.x - headP.x, ctrlP.y - headP.y, ctrlP.z - headP.z};

    if (g_calRequested.exchange(false))
    {
        g_cal.hq0 = hq;
        g_cal.c0 = cq;
        g_cal.relXR0 = relXR;
        memcpy(g_cal.B0, B, sizeof(B));
        g_haveCal = true;
        MOHW_LOG(kLogFile,
                   "CALIBRATED: relXR0=(%.3f,%.3f,%.3f) c0=(%.3f,%.3f,%.3f,%.3f) hq0=(%.3f,%.3f,%.3f,%.3f) camB0 row0=(%.3f,%.3f,%.3f) row1=(%.3f,%.3f,%.3f) row2=(%.3f,%.3f,%.3f)",
                   relXR.x, relXR.y, relXR.z, cq.x, cq.y, cq.z, cq.w, hq.x, hq.y, hq.z, hq.w, B[0][0], B[0][1], B[0][2],
                   B[1][0], B[1][1], B[1][2], B[2][0], B[2][1], B[2][2]);
    }
    if (!g_haveCal.load())
        return false;

    float S[3] = {static_cast<float>(g_signX.load()), static_cast<float>(g_signY.load()), static_cast<float>(g_signZ.load())};
    const Calib cal = g_cal;

    // A rows = world image of each XR basis vector e_i, via head-local coords at calibration.
    float A[3][3];
    Quat hqInv0 = QuatConjugate(cal.hq0);
    for (int i = 0; i < 3; ++i)
    {
        Vec3 e{i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f};
        Vec3 h = QuatRotateVector(hqInv0, e);
        float hl[3] = {S[0] * h.x, S[1] * h.y, S[2] * h.z};
        for (int k = 0; k < 3; ++k)
            A[i][k] = hl[0] * cal.B0[0][k] + hl[1] * cal.B0[1][k] + hl[2] * cal.B0[2][k];
    }
    float AT[3][3];
    Transpose3(A, AT);

    // Controller rotation delta (XR frame) -> row-form -> world.
    Quat qd = QuatMultiply(cq, QuatConjugate(cal.c0));
    float Rx[3][3], T1[3][3], Rc_w[3][3];
    QuatToRowMatrix(qd, Rx);
    MatMul3(AT, Rx, T1);
    MatMul3(T1, A, Rc_w);

    // Camera rotation since calibration, world frame: Rh_w = B0^T * B(t).
    float B0T[3][3], Rh_w[3][3], Rh_wT[3][3], Rr[3][3], RrT[3][3];
    Transpose3(cal.B0, B0T);
    MatMul3(B0T, B, Rh_w);
    Transpose3(Rh_w, Rh_wT);
    MatMul3(Rh_wT, Rc_w, Rr);
    Transpose3(Rr, RrT);

    // Weapon's camera-attached rest position (fixed head-local offset from calibration).
    Vec3 hl0 = QuatRotateVector(hqInv0, cal.relXR0);
    float posHeadCal[3] = {S[0] * hl0.x, S[1] * hl0.y, S[2] * hl0.z};
    float pos[3] = {m[12], m[13], m[14]};
    float pivot[3], Pdes[3], rel[3] = {relXR.x, relXR.y, relXR.z};
    float relW[3];
    RowMulVec(rel, A, relW);
    for (int k = 0; k < 3; ++k)
    {
        pivot[k] = pos[k] + posHeadCal[0] * B[0][k] + posHeadCal[1] * B[1][k] + posHeadCal[2] * B[2][k];
        Pdes[k] = pos[k] + relW[k];
        // Manual fine-tune (camera-local: right/up/forward), added to the desired position.
        Pdes[k] += manualRight * B[0][k] + manualUp * B[1][k] - manualFwd * B[2][k];
    }
    float v[3], vr[3];
    for (int k = 0; k < 3; ++k)
        v[k] = Pdes[k] - pos[k];
    RowMulVec(v, RrT, vr);
    for (int k = 0; k < 3; ++k)
        m[12 + k] = pivot[k] - vr[k];
    for (int i = 0; i < 3; ++i)
    {
        float nb[3];
        RowMulVec(B[i], RrT, nb);
        m[i * 4 + 0] = nb[0];
        m[i * 4 + 1] = nb[1];
        m[i * 4 + 2] = nb[2];
    }

    static std::atomic<DWORD> lastLog{0};
    DWORD tick = GetTickCount();
    DWORD prevLog = lastLog.load();
    if (tick - prevLog > 1000 && lastLog.compare_exchange_strong(prevLog, tick))
    {
        float angleDeg = 2.0f * acosf(fminf(1.0f, fabsf(qd.w))) * (180.0f / kPi);
        float camRotDeg = acosf(fmaxf(-1.0f, fminf(1.0f, (Rh_w[0][0] + Rh_w[1][1] + Rh_w[2][2] - 1.0f) * 0.5f))) * (180.0f / kPi);
        Quat qh = QuatMultiply(hq, QuatConjugate(cal.hq0));
        float headDeg = 2.0f * acosf(fminf(1.0f, fabsf(qh.w))) * (180.0f / kPi);

        // Try all 8 sign combos; report the error for each and the best.
        float bestErr = 1e9f;
        int bestMask = 0;
        float errV[8];
        char errs[160];
        int len = 0;
        for (int mask = 0; mask < 8; ++mask)
        {
            float Sm[3] = {(mask & 1) ? -1.0f : 1.0f, (mask & 2) ? -1.0f : 1.0f, (mask & 4) ? -1.0f : 1.0f};
            float e = HeadConsistencyErrorDeg(Sm, cal.hq0, hq, cal.B0, B);
            errV[mask] = e;
            if (e < bestErr)
            {
                bestErr = e;
                bestMask = mask;
            }
            len += snprintf(errs + len, sizeof(errs) - len, "%s%.0f", mask ? "," : "", e);
        }
        MOHW_LOG(kLogFile,
                   "CTRL drive v2: ctrlRot=%.1f camRot=%.1f headRot=%.1f deg | consistency err by sign mask [+++,-++,+-+,--+,++-,-+-,+--,---] = %s | best=%d (err %.1f) current signs=(%d,%d,%d) auto=%d",
                   angleDeg, camRotDeg, headDeg, errs, bestMask, bestErr, g_signX.load(), g_signY.load(), g_signZ.load(),
                   g_autoSigns.load() ? 1 : 0);

        // Only trust the fit when the head has rotated enough to be informative.
        int curMask = (g_signX.load() < 0 ? 1 : 0) | (g_signY.load() < 0 ? 2 : 0) | (g_signZ.load() < 0 ? 4 : 0);
        // Require a clear win over the CURRENT signs: pure-yaw head motion ties a mapping with its mirror
        // (observed 2026-09-20: +++ and -++ both scored 1 deg and it flipped X), so ties must not switch.
        if (g_autoSigns.load() && headDeg > 15.0f && bestErr < 6.0f && errV[curMask] - bestErr > 3.0f)
        {
            int nx = (bestMask & 1) ? -1 : 1, ny = (bestMask & 2) ? -1 : 1, nz = (bestMask & 4) ? -1 : 1;
            if (nx != g_signX.load() || ny != g_signY.load() || nz != g_signZ.load())
            {
                g_signX = nx;
                g_signY = ny;
                g_signZ = nz;
                MOHW_LOG(kLogFile, "AUTO SIGNS -> X=%d Y=%d Z=%d (head rotated %.1f deg, consistency err %.1f deg)", nx, ny, nz, headDeg, bestErr);
            }
        }
    }
    return true;
}
// DISABLED 2026-09-22 ("get rid of the camera test hook for now"): every hotkey this diagnostic used (top-row/numpad
// +/-, VK_DIVIDE role toggle, Numpad 1-9/0 offset+calibration controls, VK_OEM_5 '\' body-visibility toggle,
// VK_OEM_4/6 '[' ']' instance hide, VK_MULTIPLY dump) is now a no-op, freeing every one of those physical keys for
// other features -- most recently '\', wanted for draw_trace_diag.cpp's UI hide toggle. The controls below (per-batch
// translation offset, body/instance hide, weapon-batch role filter) are real, working functionality for tuning the
// first-person viewmodel's position -- worth resurrecting as proper controls in the mod's own settings menu once one
// exists, rather than raw always-on hotkeys that collide with everything else. NOTE: this function is still called
// every frame from HookImpl (needed for camera_matrix_test_hook.h's IsPlayerSkeletonLoaded, and to leave the
// existing default offset/selector values -- e.g. g_offRight/g_offUp's 0.5/0.5 default, g_selected's 0 -- exactly as
// they already were, since it's untested whether anything currently visible depends on them). Only the interactive
// hotkey polling was removed. The g_offRight/g_offUp test offset has since been zeroed (2026-09-26), so matched
// weapon batches now pass through unchanged.
void PollHotkeys()
{
}

void __stdcall HookImpl(void* thisPtr, void* batch, const float* matrix, uintptr_t ret)
{
    uintptr_t expectedRet = reinterpret_cast<uintptr_t>(Offset<void*>(OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL));
    if (ret != expectedRet || matrix == nullptr || batch == nullptr)
    {
        g_original(thisPtr, matrix);
        return;
    }

    g_lastViewmodelMatrixTick.store(GetTickCount(), std::memory_order_relaxed);
    PollHotkeys();

    const unsigned char* b = reinterpret_cast<const unsigned char*>(batch);
    uint32_t count = *reinterpret_cast<const uint32_t*>(b + 0x7C);
    const float* transl = reinterpret_cast<const float*>(b + 0x40);

    uintptr_t batchAddr = reinterpret_cast<uintptr_t>(batch);
    if (batchAddr != t_lastBatch)
    {
        t_lastBatch = batchAddr;
        bool startOfGroup = (count == 4) || (t_lastCount == 3) || (t_seq < 0);
        t_seq = startOfGroup ? 0 : t_seq + 1;
        t_lastCount = count;

        int line = g_seqLogLines.fetch_add(1);
        if (line < 60)
            MOHW_LOG(kLogFile, "SEQ tid=%lu seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f)", GetCurrentThreadId(), t_seq,
                       batch, count, transl[0], transl[1], transl[2]);
    }

    if (g_dumpRemaining.load() > 0 && g_dumpRemaining.fetch_sub(1) > 0)
    {
        const uintptr_t* inst = *reinterpret_cast<const uintptr_t* const*>(b + 0x78);
        MOHW_LOG(kLogFile,
                   "DUMP tid=%lu seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f) flagsByte=%02X fov=%.1f mesh0=%p",
                   GetCurrentThreadId(), t_seq, batch, count, transl[0], transl[1], transl[2],
                   static_cast<unsigned>(b[0xCC]), *reinterpret_cast<const float*>(b + 0xC4),
                   inst ? reinterpret_cast<const void*>(inst[0]) : nullptr);
    }

    int sel = g_selected.load();
    int roleMode = g_roleMode.load();
    bool patchThis;
    if (roleMode == 1)
        patchThis = (count != 3);
    else if (roleMode == 2)
        patchThis = (count == 3);
    else
        patchThis = (sel == kSelectAll) || (t_seq == sel);
    if (roleMode != 0)
        sel = 1000 + roleMode; // for the once-per-change PATCHING log below
    if (!patchThis)
    {
        g_original(thisPtr, matrix);
        return;
    }

    float m[16];
    memcpy(m, matrix, sizeof(m));
    if (kYawDegrees != 0.0f)
    {
        for (int row = 0; row < 3; ++row)
        {
            Vec3 axis{m[row * 4 + 0], m[row * 4 + 1], m[row * 4 + 2]};
            Vec3 rotated = Vec3RotateYawRight(axis, kYawDegrees);
            m[row * 4 + 0] = rotated.x;
            m[row * 4 + 1] = rotated.y;
            m[row * 4 + 2] = rotated.z;
        }
    }

    float right = g_offRight.load();
    float up = g_offUp.load();
    float fwd = g_offFwd.load();
    bool driven = g_controllerMode.load() && ApplyControllerDrive(m, right, up, fwd, b, count);
    if (!driven)
    {
        for (int k = 0; k < 3; ++k)
            m[12 + k] -= right * m[0 * 4 + k] + up * m[1 * 4 + k] + fwd * (-m[2 * 4 + k]);
    }

    ++g_patched;
    int lastLogged = g_lastLoggedSelected.load();
    if (lastLogged != sel && g_lastLoggedSelected.compare_exchange_strong(lastLogged, sel))
        MOHW_LOG(kLogFile, "PATCHING selector=%d: first hit seq=%d batch=%p count=%u transl=(%.3f,%.3f,%.3f) camera=%p", sel,
                   t_seq, batch, count, transl[0], transl[1], transl[2], thisPtr);

    g_original(thisPtr, m);
}

// SEH-safe (no C++ objects in this frame): resolves which instance of a count==3
// first-person batch this draw item is, from the item's mesh ptr and matrix ptr.
bool ResolveBodyInstance(void* item, int* outIndex, void** outMesh)
{
    __try
    {
        uintptr_t matrixPtr = *reinterpret_cast<uintptr_t*>(reinterpret_cast<unsigned char*>(item) + 0x2C);
        unsigned char* batch = reinterpret_cast<unsigned char*>(matrixPtr) - 0x10;
        uint32_t count = *reinterpret_cast<uint32_t*>(batch + 0x7C);
        uint32_t originBits = *reinterpret_cast<uint32_t*>(batch + 0x60);
        if (count != 3 || originBits == 0)
            return false;
        uintptr_t* inst = *reinterpret_cast<uintptr_t**>(batch + 0x78);
        uintptr_t mesh = *reinterpret_cast<uintptr_t*>(item);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (inst[i * 2] == mesh)
            {
                *outIndex = static_cast<int>(i);
                *outMesh = reinterpret_cast<void*>(mesh);
                return true;
            }
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void __cdecl HookedSubmit(void* item, void* drawState)
{
    int hide = g_hideInstance.load();
    bool hideAll = g_hideBodyAll.load();
    if ((hide >= 0 || hideAll) &&
        reinterpret_cast<uintptr_t>(_ReturnAddress()) ==
            reinterpret_cast<uintptr_t>(Offset<void*>(OFFSET_DRAWITEMSUBMIT_CALLER_INSTANCELOOP)))
    {
        int idx = -1;
        void* mesh = nullptr;
        if (ResolveBodyInstance(item, &idx, &mesh) && (hideAll || idx == hide))
        {
            if (hideAll)
                return;
            int last = g_lastLoggedHide.load();
            if (last != hide && g_lastLoggedHide.compare_exchange_strong(last, hide))
                MOHW_LOG(kLogFile, "HIDING count==3 batch instance %d (mesh=%p)", idx, mesh);
            return;
        }
    }
    g_originalSubmit(item, drawState);
}

// Naked stub so EBX (the caller's batch pointer, param_2 of FUN_008B2AB0) can be
// read before the compiler touches it. Original ABI: __thiscall, ECX = this,
// one stack arg (the 4x4 matrix pointer), callee pops it (RET 4).
__declspec(naked) void Detour()
{
    __asm {
        mov eax, dword ptr [esp]
        push eax
        push dword ptr [esp + 8]
        push ebx
        push ecx
        call HookImpl
        ret 4
    }
}

} // namespace

bool InstallCameraMatrixTestHook()
{
    if (g_moduleBase == 0 || !g_buildVerified)
    {
        MOHW_LOG(kLogFile, "InstallCameraMatrixTestHook: module base not resolved / build not verified yet -- refusing");
        return false;
    }

    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        MOHW_LOG(kLogFile, "MH_Initialize FAILED: %s", MH_StatusToString(initStatus));
        return false;
    }

    g_hookAddress = Offset<void*>(OFFSET_CAMERAMATRIXSET);

    MH_STATUS s = MH_CreateHook(g_hookAddress, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original));
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_CreateHook(FUN_006FC2A0 @ %p) FAILED: %s", g_hookAddress, MH_StatusToString(s));
        g_hookAddress = nullptr;
        return false;
    }
    s = MH_EnableHook(g_hookAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "MH_EnableHook FAILED: %s", MH_StatusToString(s));
        return false;
    }

    g_submitAddress = Offset<void*>(OFFSET_DRAWITEMSUBMIT);
    s = MH_CreateHook(g_submitAddress, reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&g_originalSubmit));
    if (s == MH_OK)
        s = MH_EnableHook(g_submitAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "submit hook (FUN_00723800 @ %p) FAILED: %s -- instance-hide test unavailable", g_submitAddress,
                   MH_StatusToString(s));
        g_submitAddress = nullptr;
    }
    else
    {
        MOHW_LOG(kLogFile, "instance-hide hook installed @ %p ('[' / ']' step: -1 off, 0..3 = hide that instance of the count==3 batch)",
                   g_submitAddress);
    }

    MOHW_LOG(kLogFile,
               "Camera matrix test hook installed @ %p -- ret=%p yawed %.0f deg. '+'/'-' (main row or numpad) steps the "
               "selector: -1 = all 12 batches, 0..11 = one batch by group index (0=count4 batch, 1-9=singles, 11=count3)",
               g_hookAddress, Offset<void*>(OFFSET_CAMERAMATRIX_CALLER_VIEWMODEL), kYawDegrees);
    return true;
}

void RemoveCameraMatrixTestHook()
{
    if (g_submitAddress)
    {
        MH_DisableHook(g_submitAddress);
        MH_RemoveHook(g_submitAddress);
        g_submitAddress = nullptr;
    }
    if (g_hookAddress)
    {
        MH_DisableHook(g_hookAddress);
        MH_RemoveHook(g_hookAddress);
        g_hookAddress = nullptr;
    }
}

bool IsPlayerSkeletonLoaded()
{
    constexpr DWORD kRecentMs = 500;
    DWORD last = g_lastViewmodelMatrixTick.load(std::memory_order_relaxed);
    if (last == 0)
        return false;
    return (GetTickCount() - last) <= kRecentMs; // unsigned subtraction wraps correctly across GetTickCount() rollover
}

} // namespace mohw
