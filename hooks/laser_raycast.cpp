#include "laser_raycast.h"

#include "bullet_raycast_redirect_hook.h"
#include "render_pose_stamp.h"
#include "weapon_identity.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"
#include "../sdk/vr_math.h"
#include "../sdk/motion_controller_aim.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_laser.log";
constexpr unsigned kMaxHits = 5;
constexpr uint32_t kHitSize = 0x40;       // RayCastHit (sdk/mohw_offsets.h OFFSET_GAMEWORLD_INSTANCE_PTR comment)
constexpr uint32_t kHitObject = 0x24;
constexpr uint32_t kHitObjectFlags = 0x08;
constexpr uint32_t kSkipHitFlag = 0x8000000; // getTargetPosition skips hits whose object carries this
constexpr unsigned long long kFreshMs = 300; // a hit older than this isn't shown (the laser stopped running)

using RayCastFn = unsigned int(__thiscall*)(void* world, const char* ident, int rayCastTest, float* start, float* end,
                                            void* hits, unsigned int maxHitCount, unsigned int materialFlags,
                                            unsigned int flags, void* excluded);
using GetterFn = uint32_t(__thiscall*)(void* self);

// eastl::fixed_vector's header as RayCast reads it: begin, end, capacity end.
struct ExclusionList
{
    uint32_t* begin;
    uint32_t* end;
    uint32_t* capacityEnd;
};

std::atomic<float> g_hitTracking{0.0f};
std::atomic<unsigned long long> g_hitMs{0};
// The laser's world point for the shots (GetLaserAimPoint): its hit, or its end when nothing's hit.
std::atomic<float> g_aimX{0.0f}, g_aimY{0.0f}, g_aimZ{0.0f};
std::atomic<unsigned long long> g_aimMs{0};
// The view direction is MINUS the camera's row 2 (row 2 = back, see LaserOnCommit's toWorld; an earlier "dot ~0.97
// with row 2" reading came from fire-hook calls that weren't the player's shots). No automatic flip: one keyed to the
// fire hook's directions flipped the laser backwards and back again within 8 s (live 13:34:39 / 13:34:47) -- that hook
// also sees calls that aren't the player's shots. LaserNoteShotDirection only logs.
std::atomic<float> g_forwardX{0.0f}, g_forwardY{0.0f}, g_forwardZ{0.0f}; // the last camera view direction seen
std::atomic<float> g_gameCamX{0.0f}, g_gameCamY{0.0f}, g_gameCamZ{0.0f}; // the game's own camera: the soldier's eye
// OWN CAPSULE (2026-10-15, live): most short hits were a CharacterPhysicsEntity (the hit record's +0x20) about 0.6 m
// from the game camera -- the player's own character collision capsule, which the soldier entity in the exclusion list
// doesn't cover. A CharacterPhysicsEntity hit within kOwnCapsuleRadius of the soldier's eye axis is the player's: it's
// skipped and that entity joins the exclusion list (checked to still be one every cast).
constexpr float kOwnCapsuleRadius = 0.9f;
std::atomic<uint32_t> g_ownCapsule{0};
// OPEN (2026-10-15): most casts in a live session stopped 0.3-1.4 m from the hand, in every direction, on two
// materials flagged 0x01000000 (0x0100063F / 0x01000EB0) that world surfaces don't carry -- from the very moment of a
// recenter, so not the hands drifting into the soldier. Every hit is now logged with the object behind it (the hit
// record's +0x20, by its reflected type) and its +0x28/+0x2C fields, to see what it is before filtering anything.
int g_frame = 0;     // main thread
int g_castLogs = 0;  // main thread

bool InModule(uintptr_t p)
{
    return p >= g_moduleBase && p < g_moduleBase + MOHW_MP_SIZE;
}

bool ReadU32(uintptr_t address, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CopyFloats16(float* dst, const float* src)
{
    __try
    {
        memcpy(dst, src, 16 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A virtual getter, called the way the game does (ECX = self). A plain field read ("mov eax,[ecx+disp]; ret") is read
// instead of called.
uint32_t CallGetter(uintptr_t self, uintptr_t fn)
{
    unsigned char code[7] = {};
    __try
    {
        memcpy(code, reinterpret_cast<const void*>(fn), sizeof(code));
        uint32_t value = 0;
        if (code[0] == 0x8B && code[1] == 0x41 && code[3] == 0xC3)
            return ReadU32(self + code[2], &value) ? value : 0;
        if (code[0] == 0x8B && code[1] == 0x81 && code[6] == 0xC3)
        {
            uint32_t disp = 0;
            memcpy(&disp, code + 2, 4);
            return ReadU32(self + disp, &value) ? value : 0;
        }
        return reinterpret_cast<GetterFn>(fn)(reinterpret_cast<void*>(self));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// The player's soldier and weapon, excluded from the laser: ClientSoldierWeapon +0x42C -> a handle to its soldier
// (entity = [handle] - 4). The soldier entity itself, as the bullet's own raycast excludes its shooter (FUN_00814480
// copies [handle] - 4 entries -- the first live run, without it, only managed the weapon); plus what
// getTargetPosition excludes: the soldier's sub-object at +0xB0, vtable slot +0x38, and ClientWeapon (+0x440) +0xB4.
// Returns how many it found.
int BuildExclusions(uint32_t* storage)
{
    uintptr_t owner = GetEquippedSoldierWeapon();
    if (!owner)
        return 0;
    int n = 0;
    uint32_t ref = 0, soldierPlus4 = 0, vtable = 0, getter = 0;
    if (ReadU32(owner + 0x42C, &ref) && ref && ReadU32(ref, &soldierPlus4) && soldierPlus4)
    {
        storage[n++] = soldierPlus4 - 4; // the soldier entity
        uintptr_t sub = soldierPlus4 - 4 + 0xB0;
        if (ReadU32(sub, &vtable) && InModule(vtable) && ReadU32(vtable + 0x38, &getter) && InModule(getter))
        {
            uint32_t physics = CallGetter(sub, getter);
            if (physics)
                storage[n++] = physics;
        }
    }
    uint32_t clientWeapon = 0, physics = 0;
    if (ReadU32(owner + 0x440, &clientWeapon) && clientWeapon && ReadU32(clientWeapon + 0xB4, &physics) && physics)
        storage[n++] = physics;
    return n;
}

unsigned int SafeRayCast(RayCastFn fn, void* world, float* start, float* end, void* hits, ExclusionList* excluded,
                         bool* ok)
{
    __try
    {
        *ok = true;
        // The PROJECTILE query bullets and decals use (material flags 0x01000000, flags 0: "BulletEntity
        // updateTransformSync", "MissileEntity applyDecal"), so the laser stops where bullets and their decals land.
        // (The game's other kind, the visibility query -- 0x8000000 / 4: AI line of sight, getTargetPosition -- was
        // worse live.) A long ray needs the shrinking passes in LaserOnCommit: the query reports a hit, not the nearest.
        return fn(world, "MOHWVR laser", 0, start, end, hits, kMaxHits, 0x01000000, 0, excluded);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *ok = false;
        return 0;
    }
}

} // namespace

void LaserCaptureGameCamera(const float* gameTransform)
{
    float m[16];
    if (!gameTransform || !CopyFloats16(m, gameTransform))
        return;
    g_forwardX.store(-m[8], std::memory_order_relaxed);
    g_forwardY.store(-m[9], std::memory_order_relaxed);
    g_forwardZ.store(-m[10], std::memory_order_relaxed);
    g_gameCamX.store(m[12], std::memory_order_relaxed);
    g_gameCamY.store(m[13], std::memory_order_relaxed);
    g_gameCamZ.store(m[14], std::memory_order_relaxed);
}

void LaserOnCommit(const float* renderTransform)
{
    if (g_drivingExe || !renderTransform || (!GetSightDotEnabled() && !GetLaserBeamEnabled()) ||
        !GetWeaponDriveEnabled())
        return;
    if (++g_frame < GetLaserRaycastEveryFrames())
        return;
    g_frame = 0;

    float m[16];
    if (!CopyFloats16(m, renderTransform))
        return;
    // RENDER MAPPING (2026-10-15): the laser's world ray is the drawn beam carried into the world through the camera
    // being rendered -- a tracking-space point relative to the head, in the head's frame (OpenVR: x right, y up,
    // z back), lands at camera position + row0*x + row1*y + row2*z (see toWorld below for the axes). The first version
    // went through the head-look mapping instead (camera forward turned by the controller's yaw/pitch) and ran a few
    // degrees off the beam (live: along the edge of a wall the beam was visibly on).
    mohwvr::ipc::HeadPoseBlock head{};
    mohwvr::ipc::ControllerPoseBlock hand{};
    if (!GetHeadPose(&head) || !GetRightControllerPose(&hand) || !hand.ready)
        return;
    // FRAME POSE (2026-10-15): the head pose this frame is SHOWN as rendered from -- the render-pose stamp, which the
    // compositor reprojects from -- not the live head pose. They differ by the rotation smoothing's lag and anything
    // the game adds to the camera (live: shots "a little higher than the dot at a distance", the stamp's facing
    // 0.4-0.9 deg off the live head while firing). Cast through the live pose, the hit lands that far off the drawn dot.
    Quat headQ{head.orientationX, head.orientationY, head.orientationZ, head.orientationW};
    float headP[3] = {head.positionX, head.positionY, head.positionZ};
    float stampQ[4], stampP[3];
    bool stampPosValid = false;
    if (GetCommittedFrameRenderPose(stampQ, stampP, &stampPosValid))
    {
        headQ = Quat{stampQ[0], stampQ[1], stampQ[2], stampQ[3]};
        if (stampPosValid)
            memcpy(headP, stampP, sizeof(headP));
    }
    Quat headInverse = QuatConjugate(headQ);
    Quat aim = ApplyControllerAimOffset(Quat{hand.orientationX, hand.orientationY, hand.orientationZ, hand.orientationW});
    Vec3 forwardLocal;
    forwardLocal.x = 0.0f;
    forwardLocal.y = 0.0f;
    forwardLocal.z = -1.0f;
    Vec3 handRelative;
    handRelative.x = hand.positionX - headP[0];
    handRelative.y = hand.positionY - headP[1];
    handRelative.z = hand.positionZ - headP[2];
    Vec3 handInHead = QuatRotateVector(headInverse, handRelative);
    Vec3 dirInHead = QuatRotateVector(headInverse, QuatRotateVector(aim, forwardLocal));
    // The camera's rows line up with the head's axes AS THEY ARE: row 0 = the head's +x (right), row 1 = +y (up),
    // row 2 = +z (back) -- the view looks down -row 2. Live ground truth 2026-10-15 18:17: the player's own bullets
    // left the camera along (-0.44, 0.00, 0.90) while a "left / up / forward = view" reading cast the laser along
    // (0.42, -0.09, -0.91) -- straight behind the player, so it hit whatever was back there ("through walls", "stops
    // in mid air"). Same convention as the driving camera (hooks/driving_camera_hook.cpp: the head rotation carries
    // over as-is).
    auto toWorld = [&m](const Vec3& v) {
        Vec3 w;
        w.x = m[0] * v.x + m[4] * v.y + m[8] * v.z;
        w.y = m[1] * v.x + m[5] * v.y + m[9] * v.z;
        w.z = m[2] * v.x + m[6] * v.y + m[10] * v.z;
        return w;
    };
    float scale = GetHeadPositionScale();
    if (!(scale > 0.01f))
        scale = 1.0f;
    Vec3 dir = toWorld(dirInHead);
    float dirLength = sqrtf(VecDot(dir, dir));
    if (!(dirLength > 1e-4f))
        return;
    dir = VecScale(dir, 1.0f / dirLength);
    Vec3 handWorld = toWorld(handInHead);
    Vec3 origin;
    origin.x = m[12] + handWorld.x * scale;
    origin.y = m[13] + handWorld.y * scale;
    origin.z = m[14] + handWorld.z * scale;
    if (!(scale > 0.01f))
        scale = 1.0f;
    float range = GetSightZeroDistance() * scale; // world meters, from the hand
    // The laser starts LaserStart forward of the hand (the muzzle), so it can't hit the gun; distances stay from the hand.
    float startOffset = GetLaserStartMeters() * scale;
    if (startOffset > range - 0.1f)
        startOffset = range - 0.1f;

    uint32_t storage[32] = {};
    int excludedCount = BuildExclusions(storage);
    uint32_t ownCapsule = g_ownCapsule.load(std::memory_order_relaxed);
    char capsuleType[64];
    if (ownCapsule && GetReflectedTypeName(ownCapsule, capsuleType, sizeof(capsuleType)) &&
        strcmp(capsuleType, "CharacterPhysicsEntity") == 0)
        storage[excludedCount++] = ownCapsule;
    else if (ownCapsule)
        g_ownCapsule.store(0, std::memory_order_relaxed); // gone (a new level)
    ExclusionList excluded{storage, storage + excludedCount, storage + 32};
    float gameCam[3] = {g_gameCamX.load(std::memory_order_relaxed), g_gameCamY.load(std::memory_order_relaxed),
                        g_gameCamZ.load(std::memory_order_relaxed)};

    uint32_t world = 0;
    RayCastFn fn = reinterpret_cast<RayCastFn>(GetGameWorldRayCastOriginal());
    if (!fn || !ReadU32(g_baseDelta + OFFSET_GAMEWORLD_INSTANCE_PTR, &world) || !world)
        return;

    // BULLET STEPPING (2026-10-15, the user's call: copy the bullet exactly): BulletEntity updateTransformSync
    // (FUN_0091cd60) moves ~11.6 m a tick and raycasts just that segment (test 0, 5 hits, 0x01000000, flags 0), taking
    // the first returned hit its handler accepts. The laser steps the same segments out from its start and takes the
    // first returned hit, skipping only the player's own capsule / skip-flagged ones. Past ~116 m, 50 m steps.
    constexpr float kBulletStep = 11.6f;
    constexpr float kFarStep = 50.0f;
    constexpr int kMaxCasts = 24;
    alignas(16) float start[4] = {origin.x + dir.x * startOffset, origin.y + dir.y * startOffset,
                                  origin.z + dir.z * startOffset, 0.0f};
    alignas(16) float end[4] = {origin.x + dir.x * range, origin.y + dir.y * range, origin.z + dir.z * range, 0.0f};
    alignas(16) float segStart[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) float segEnd[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    alignas(16) unsigned char hits[kMaxHits * kHitSize];
    float best = -1.0f;
    float bestPos[3] = {};
    uint32_t bestObject = 0, bestFlags = 0, bestEntity = 0;
    int casts = 0;
    bool ok = true;
    float from = startOffset;
    while (from < range && casts < kMaxCasts && ok && best < 0.0f)
    {
        float to = from + (from < 10.0f * kBulletStep * scale ? kBulletStep * scale : kFarStep * scale);
        if (to > range)
            to = range;
        for (int k = 0; k < 3; ++k)
        {
            const float* d = &dir.x;
            segStart[k] = (&origin.x)[k] + d[k] * from;
            segEnd[k] = (&origin.x)[k] + d[k] * to;
        }
        memset(hits, 0, sizeof(hits));
        for (unsigned i = 0; i < kMaxHits; ++i)
        {
            uint32_t none = 0xFFFFFFFFu;
            memcpy(hits + i * kHitSize + 0x28, &none, 4);
            memcpy(hits + i * kHitSize + 0x2C, &none, 4);
        }
        unsigned int count = SafeRayCast(fn, reinterpret_cast<void*>(world), segStart, segEnd, hits, &excluded, &ok);
        ++casts;
        for (unsigned i = 0; ok && i < count && i < kMaxHits; ++i) // in returned order, as the bullet does
        {
            const unsigned char* hit = hits + i * kHitSize;
            uint32_t object = 0, flags = 0, entity = 0;
            float pos[3];
            memcpy(&object, hit + kHitObject, 4);
            memcpy(&entity, hit + 0x20, 4);
            memcpy(pos, hit, sizeof(pos));
            if (object && ReadU32(object + kHitObjectFlags, &flags) && (flags & kSkipHitFlag))
                continue;
            char entityType[64];
            if (entity && GetReflectedTypeName(entity, entityType, sizeof(entityType)) &&
                strcmp(entityType, "CharacterPhysicsEntity") == 0)
            {
                float hx = pos[0] - gameCam[0], hz = pos[2] - gameCam[2];
                float radius = kOwnCapsuleRadius * scale;
                if (entity == g_ownCapsule.load(std::memory_order_relaxed) || hx * hx + hz * hz < radius * radius)
                {
                    if (g_ownCapsule.exchange(entity, std::memory_order_relaxed) != entity)
                        MOHW_LOG(kLogFile, "own character capsule: %08X (excluded from now on)", entity);
                    continue;
                }
            }
            float dx = pos[0] - origin.x, dy = pos[1] - origin.y, dz = pos[2] - origin.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(dist))
                continue;
            best = dist;
            memcpy(bestPos, pos, sizeof(bestPos));
            bestObject = object;
            bestFlags = flags;
            bestEntity = entity;
            break;
        }
        from = to;
    }

    // What it hit, every 2 s (to check the dot against the scene).
    static unsigned long long nextHitLogMs = 0;
    unsigned long long nowMs = GetTickCount64();
    if (nowMs >= nextHitLogMs)
    {
        nextHitLogMs = nowMs + 2000;
        char entityType[96] = "?";
        if (bestEntity)
            GetReflectedTypeName(bestEntity, entityType, sizeof(entityType));
        MOHW_LOG(kLogFile, "laser: %.2f m at (%.2f, %.2f, %.2f) after %d bullet-step cast(s), material %08X flags %08X, %s; from the hand (%.2f, %.2f, %.2f) along (%.3f, %.3f, %.3f), camera (%.2f, %.2f, %.2f), start +%.2f m, %d excluded",
                  best, bestPos[0], bestPos[1], bestPos[2], casts, bestObject, bestFlags, entityType, origin.x,
                  origin.y, origin.z, dir.x, dir.y, dir.z, m[12], m[13], m[14], startOffset, excludedCount);
    }
    if (best >= 0.0f)
    {
        g_hitTracking.store(best / scale, std::memory_order_relaxed);
        g_hitMs.store(nowMs, std::memory_order_relaxed);
        g_aimX.store(bestPos[0], std::memory_order_relaxed);
        g_aimY.store(bestPos[1], std::memory_order_relaxed);
        g_aimZ.store(bestPos[2], std::memory_order_relaxed);
    }
    else
    {
        g_hitMs.store(0, std::memory_order_relaxed); // nothing within range: the dot at the max range
        g_aimX.store(end[0], std::memory_order_relaxed);
        g_aimY.store(end[1], std::memory_order_relaxed);
        g_aimZ.store(end[2], std::memory_order_relaxed);
    }
    g_aimMs.store(nowMs, std::memory_order_relaxed);

    if (!ok && g_castLogs < 20)
    {
        ++g_castLogs;
        MOHW_LOG(kLogFile, "laser raycast FAULTED (from (%.2f, %.2f, %.2f), %d excluded)", start[0], start[1], start[2],
                  excludedCount);
    }
}


void LaserNoteShotDirection(float x, float y, float z)
{
    float fx = g_forwardX.load(std::memory_order_relaxed), fy = g_forwardY.load(std::memory_order_relaxed),
          fz = g_forwardZ.load(std::memory_order_relaxed);
    float len = sqrtf(x * x + y * y + z * z) * sqrtf(fx * fx + fy * fy + fz * fz);
    if (!(len > 1e-6f))
        return;
    float dot = (x * fx + y * fy + z * fz) / len;
    static int logs = 0;
    if (logs < 6 || (dot < -0.5f && logs < 30))
    {
        ++logs;
        MOHW_LOG(kLogFile, "fire hook direction vs camera forward: dot %.3f (logged only)", dot);
    }
}

void LaserNoteBulletHits(const void* hits, unsigned int count, const float* segmentStart, const float* segmentEnd)
{
    static unsigned long long windowSecond = 0;
    static int linesThisSecond = 0;
    unsigned long long now = GetTickCount64();
    if (now / 1000 != windowSecond)
    {
        windowSecond = now / 1000;
        linesThisSecond = 0;
    }
    if (!hits || !segmentStart || !segmentEnd || linesThisSecond >= 6)
        return;
    ++linesThisSecond;
    float aim[3] = {};
    bool haveAim = GetLaserAimPoint(aim);
    float laserDistance = 0.0f;
    bool laserHit = GetLaserHitDistance(&laserDistance);
    MOHW_LOG(kLogFile, "BULLET segment (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f): %u hit(s); laser point %s(%.2f, %.2f, %.2f)%s",
              segmentStart[0], segmentStart[1], segmentStart[2], segmentEnd[0], segmentEnd[1], segmentEnd[2], count,
              haveAim ? "" : "[none] ", aim[0], aim[1], aim[2], laserHit ? " (a hit)" : " (max range, no hit)");
    const unsigned char* bytes = static_cast<const unsigned char*>(hits);
    for (unsigned i = 0; i < count && i < kMaxHits; ++i)
    {
        const unsigned char* hit = bytes + i * kHitSize;
        float pos[3];
        uint32_t entity = 0, material = 0, materialFlags = 0;
        memcpy(pos, hit, sizeof(pos));
        memcpy(&entity, hit + 0x20, 4);
        memcpy(&material, hit + kHitObject, 4);
        if (material)
            ReadU32(material + kHitObjectFlags, &materialFlags);
        char entityType[64] = "?";
        if (entity)
            GetReflectedTypeName(entity, entityType, sizeof(entityType));
        float dx = pos[0] - aim[0], dy = pos[1] - aim[1], dz = pos[2] - aim[2];
        MOHW_LOG(kLogFile, "    bullet hit %u at (%.2f, %.2f, %.2f), %.2f m from the laser point, material flags %08X, %s", i,
                  pos[0], pos[1], pos[2], sqrtf(dx * dx + dy * dy + dz * dz), materialFlags, entityType);
    }
}

bool GetLaserAimPoint(float worldPoint[3])
{
    unsigned long long at = g_aimMs.load(std::memory_order_relaxed);
    if (at == 0 || GetTickCount64() - at > kFreshMs)
        return false;
    worldPoint[0] = g_aimX.load(std::memory_order_relaxed);
    worldPoint[1] = g_aimY.load(std::memory_order_relaxed);
    worldPoint[2] = g_aimZ.load(std::memory_order_relaxed);
    return true;
}

bool GetLaserHitDistance(float* trackingMeters)
{
    unsigned long long at = g_hitMs.load(std::memory_order_relaxed);
    if (at == 0 || GetTickCount64() - at > kFreshMs)
        return false;
    *trackingMeters = g_hitTracking.load(std::memory_order_relaxed);
    return true;
}

} // namespace mohw
