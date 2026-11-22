#include "boat_seat.h"

#include "aiming_controller_hook.h"
#include "alternating_eye.h"
#include "boat_math.h"
#include "companion_bridge.h"
#include "input_restriction.h"
#include "native_stereo.h"
#include "player_control.h"
#include "weapon_identity.h"
#include "../openvr_direct/vr_input.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_common.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"
#include "../shared/ipc_protocol.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_boat.log";
constexpr const char* kLevel = "030_Basilan";
constexpr const char* kVehicle = "MOHW/Vehicles/rhib/RHIB_SP";
constexpr uint32_t kDriverOrder = 0; // EntryOrderNumber
constexpr uint32_t kGunnerOrder = 1;

constexpr unsigned long long kSeatFreshMs = 1000;
constexpr unsigned long long kFrameFreshMs = 250;
constexpr unsigned long long kHandsFreshMs = 250;
constexpr unsigned long long kPausedAfterMs = 300;
constexpr unsigned long long kTurnedByStickMs = 400; // past the stick's release: the turret eases out over 0.2 s
constexpr float kTurnStick = 0.25f;
constexpr float kStraightAheadSmoothingSeconds = 0.08f; // the boat's transform steps at the simulation's 30 Hz
constexpr float kMaxHeadMetres = 1.5f;
constexpr float kRawCameraMatchMetres = 0.5f;

std::atomic<int> g_seat{0};
std::atomic<uintptr_t> g_vehicle{0};
std::atomic<unsigned long long> g_seatMs{0};
std::atomic<unsigned long long> g_cameraChangedMs{0};

// What a commit leaves for the other threads, and what the submit thread leaves for a commit. Guarded by g_mutex.
struct Frame
{
    unsigned long long ms = 0;
    bool valid = false;
    Rigid delta{};
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float position[3] = {};
    bool haveStick = false;
    float stick[2] = {};
};
struct Hands
{
    unsigned long long ms = 0;
    bool gripping[2] = {};
    float position[2][3] = {};
};
std::mutex g_mutex;
Frame g_frame;
Hands g_hands;

bool Copy(void* to, uintptr_t from, size_t n)
{
    if (from < 0x10000 || from >= 0xFFFF0000u)
        return false;
    __try
    {
        memcpy(to, reinterpret_cast<const void*>(from), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadU32(uintptr_t address, uint32_t* out)
{
    return Copy(out, address, sizeof(*out));
}

// A CString field: a pointer to the characters.
bool ReadString(uintptr_t field, char* out, size_t size)
{
    uint32_t text = 0;
    if (!ReadU32(field, &text) || !text)
        return false;
    for (size_t i = 0; i + 1 < size; ++i)
    {
        if (!Copy(out + i, text + i, 1))
            return false;
        if (out[i] == '\0')
            return i > 0;
    }
    return false;
}

bool ReadLevelName(char* out, size_t size)
{
    uint32_t context = 0, level = 0;
    return ReadU32(g_baseDelta + OFFSET_CLIENTGAMECONTEXT, &context) && context &&
           ReadU32(context + CLIENTGAMECONTEXT_LEVEL, &level) && level && ReadString(level + CLIENTLEVEL_NAME, out, size);
}

// A vehicle and the player's entry in it: what it is and the entry's order number.
bool ReadVehicleSeat(uintptr_t entity, uint32_t entryId, char* type, size_t typeSize, uint32_t* order)
{
    char className[64];
    if (!entity || !GetReflectedTypeName(entity, className, sizeof(className)) || strcmp(className, "ClientVehicleEntity") != 0)
        return false;
    uint32_t data = 0, begin = 0, end = 0, component = 0, componentData = 0;
    return ReadU32(entity + ENTITY_DATA, &data) && data && ReadString(data + VEHICLEDATA_CONTROLLABLETYPE, type, typeSize) &&
           ReadU32(entity + CONTROLLABLE_ENTRIES, &begin) && ReadU32(entity + CONTROLLABLE_ENTRIES + 4, &end) && begin &&
           end > begin && entryId < (end - begin) / 4 && ReadU32(begin + entryId * 4, &component) && component &&
           ReadU32(component + COMPONENT_DATA, &componentData) && componentData &&
           ReadU32(componentData + ENTRYDATA_ORDERNUMBER, order);
}

// An entity's world transform, read as FUN_00908630 and FUN_00851960 do (never called).
bool ReadEntityTransform(uintptr_t entity, float out[16])
{
    uint32_t flags = 0, where = 0;
    if (!entity || !ReadU32(entity + ENTITY_FLAGS, &flags) || !ReadU32(entity + ENTITY_TRANSFORM, &where) || !where)
        return false;
    uintptr_t at = where;
    if (flags >> 20 & 1)
    {
        uint8_t count = 0;
        if (!Copy(&count, at + 6, 1))
            return false;
        at += 0x10 + count * 0x50u;
    }
    return Copy(out, at, sizeof(float) * 16);
}

bool UnitRows(const float* m)
{
    for (int i = 0; i < 3; ++i)
    {
        const float* r = m + i * 4;
        const float d = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (!(d > 0.81f && d < 1.21f))
            return false;
    }
    return std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]);
}

double Seconds()
{
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(frequency.QuadPart);
}

const char* SeatName(BoatSeat seat)
{
    return seat == BoatSeat::Driver ? "the wheel" : seat == BoatSeat::Gunner ? "the Mk19" : "none";
}

void Publish(const Frame& frame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame = frame;
}

bool LatestFrame(Frame* out)
{
    if (GetBoatSeat() == BoatSeat::None)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_frame.valid || GetTickCount64() - g_frame.ms > kFrameFreshMs)
        return false;
    *out = g_frame;
    return true;
}

// The gripping hands' middle in the base frame, and which they are.
int GrippingHands(const boat::Reference& reference, float scale, float middle[3])
{
    Hands hands;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hands = g_hands;
    }
    if (hands.ms == 0 || GetTickCount64() - hands.ms > kHandsFreshMs)
        return 0;
    float sum[3] = {};
    int mask = 0, count = 0;
    for (int h = 0; h < 2; ++h)
    {
        if (!hands.gripping[h])
            continue;
        mask |= 1 << h;
        ++count;
        for (int k = 0; k < 3; ++k)
            sum[k] += hands.position[h][k];
    }
    if (count == 0)
        return 0;
    float moved[3];
    for (int k = 0; k < 3; ++k)
        moved[k] = (sum[k] / static_cast<float>(count) - reference.position[k]) * scale;
    boat::IntoReference(moved, reference.yaw, middle);
    return mask;
}

} // namespace

void BoatSeatOnPresent()
{
    if (g_drivingExe || g_moduleBase == 0 || !g_buildVerified)
        return;
    const unsigned long long now = GetTickCount64();
    PlayerControl control{};
    BoatSeat seat = BoatSeat::None;
    uintptr_t vehicle = 0;
    char type[128] = "", level[128] = "";
    uint32_t order = 0;
    bool inVehicle = false;
    if (GetPlayerControl(&control))
    {
        if (ReadVehicleSeat(control.controlled, control.controlledEntry, type, sizeof(type), &order))
        {
            vehicle = control.controlled;
            inVehicle = true;
        }
        else if (ReadVehicleSeat(control.attached, control.attachedEntry, type, sizeof(type), &order))
        {
            // The game's own rule (FUN_00909f70): the soldier controlled but attached = the attached one's entry.
            vehicle = control.attached;
            inVehicle = true;
        }
    }
    if (inVehicle)
    {
        if (!ReadLevelName(level, sizeof(level)))
            level[0] = '\0';
        if (_stricmp(type, kVehicle) == 0 && strstr(level, kLevel) != nullptr)
            seat = order == kDriverOrder ? BoatSeat::Driver : order == kGunnerOrder ? BoatSeat::Gunner : BoatSeat::None;
    }

    static uintptr_t lastVehicle = 0;
    static uint32_t lastOrder = 0;
    static int lastSeat = -1;
    static bool lastHandled = false;
    const bool handled = seat != BoatSeat::None && GetBoatSixDof();
    if (vehicle != lastVehicle || (inVehicle && order != lastOrder) || static_cast<int>(seat) != lastSeat ||
        handled != lastHandled) // the toggles too, flipped in the seat
    {
        lastHandled = handled;
        lastVehicle = vehicle;
        lastOrder = order;
        lastSeat = static_cast<int>(seat);
        if (inVehicle)
            MOHW_LOG_ALWAYS(kLogFile,
                            "in a vehicle: \"%s\" entry order %u, level \"%s\" -> boat seat: %s (specialized sequences "
                            "%d; Zodiac Sequence: 6DoF in use %d, gun aim %d, wheel %d; aim updates %s)",
                            type, order, level, SeatName(seat), GetSpecializedSequenceMotion() ? 1 : 0, handled ? 1 : 0,
                            GetBoatGunAim() ? 1 : 0, GetBoatWheel() ? 1 : 0, IsPlayerAimActive() ? "running" : "stopped");
        else
            MOHW_LOG_ALWAYS(kLogFile, "not in a vehicle -> boat seat: none");
    }
    g_vehicle.store(vehicle, std::memory_order_relaxed);
    g_seat.store(static_cast<int>(seat), std::memory_order_relaxed);
    g_seatMs.store(now, std::memory_order_release);
    // The soldier's aim may stop in a seat, and then it can't see look taken: getting up has to re-zero the head's yaw.
    if (handled)
        NoteLookTaken();
}

BoatSeat GetBoatSeatRaw()
{
    const unsigned long long at = g_seatMs.load(std::memory_order_acquire);
    if (at == 0 || GetTickCount64() - at > kSeatFreshMs)
        return BoatSeat::None;
    return static_cast<BoatSeat>(g_seat.load(std::memory_order_relaxed));
}

BoatSeat GetBoatSeat()
{
    const BoatSeat seat = GetBoatSeatRaw();
    return seat != BoatSeat::None && GetBoatSixDof() ? seat : BoatSeat::None;
}

bool IsBoatPaused()
{
    const unsigned long long at = g_cameraChangedMs.load(std::memory_order_relaxed);
    return at != 0 && GetTickCount64() - at > kPausedAfterMs;
}

void NoteBoatHands(const bool gripping[2], const float position[2][3])
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hands.ms = GetTickCount64();
    memcpy(g_hands.gripping, gripping, sizeof(g_hands.gripping));
    memcpy(g_hands.position, position, sizeof(g_hands.position));
}

bool ApplyBoatHead(float* view, const float gameCamera[16])
{
    // The commit hook's thread only.
    static BoatSeat lastSeat = BoatSeat::None;
    static bool haveReference = false;
    static unsigned generation = 0;
    static boat::Reference reference;
    static float seatTurn = 0.0f;      // straight ahead, from the boat's heading
    static float straightAhead = 0.0f; // that in the world, smoothed
    static float lastTurretYaw = 0.0f;
    static unsigned long long turnedByStickUntil = 0;
    static double lastSeconds = 0.0;
    static float lastRaw[16] = {};
    static boat::Lever lever;
    static boat::TurretAxis yawAxis, pitchAxis;
    static unsigned long long nextLogMs = 0;

    const BoatSeat seat = GetBoatSeat();
    const auto letGo = [&]() {
        lever.Release();
        yawAxis.Forget();
        pitchAxis.Forget();
    };
    if (seat != lastSeat)
    {
        lastSeat = seat;
        haveReference = false;
        letGo();
        g_cameraChangedMs.store(0, std::memory_order_relaxed);
        Publish(Frame{});
    }
    if (!view || !gameCamera || seat == BoatSeat::None)
        return false;
    if (!UnitRows(gameCamera))
    {
        Publish(Frame{});
        return false;
    }

    const unsigned long long now = GetTickCount64();
    // The params' raw camera: what the first-person batches are drawn from, and nothing in the commit hook edits it.
    float raw[16];
    memcpy(raw, view + VIEWPARAMS_RAW_CAMERA / sizeof(float), sizeof(raw));
    const float dx = raw[12] - gameCamera[12], dy = raw[13] - gameCamera[13], dz = raw[14] - gameCamera[14];
    if (!UnitRows(raw) || !(dx * dx + dy * dy + dz * dz < kRawCameraMatchMetres * kRawCameraMatchMetres))
        memcpy(raw, gameCamera, sizeof(raw));
    if (memcmp(raw, lastRaw, sizeof(raw)) != 0 || g_cameraChangedMs.load(std::memory_order_relaxed) == 0)
    {
        memcpy(lastRaw, raw, sizeof(raw));
        g_cameraChangedMs.store(now, std::memory_order_relaxed);
    }

    mohwvr::ipc::HeadPoseBlock pose{};
    if (IsLookRestricted() || !GetHeadPose(&pose))
    {
        // The game has look: its camera as it is, and where the head faces when it's given back is straight ahead.
        haveReference = false;
        letGo();
        Publish(Frame{});
        return false;
    }
    const Quat head{pose.orientationX, pose.orientationY, pose.orientationZ, pose.orientationW};
    const float headPosition[3] = {pose.positionX, pose.positionY, pose.positionZ};

    const bool gunner = seat == BoatSeat::Gunner;
    const Rigid game = FromMatrix(gameCamera);
    const Rigid gun = boat::GunFromCamera(game);
    const float facing = boat::HeadingOfBack(gunner ? gun.R[2] : game.R[2]);
    float boatYaw = facing;
    float world[16];
    const bool haveBoat = ReadEntityTransform(g_vehicle.load(std::memory_order_relaxed), world) && UnitRows(world) &&
                          world[8] * world[8] + world[10] * world[10] > 0.1f;
    if (haveBoat)
        boatYaw = atan2f(world[8], world[10]); // either sign of the bow will do: seatTurn is measured from it
    const float turretYaw = WrapAngleSigned(facing - boatYaw);

    const double seconds = Seconds();
    const unsigned recenter = GetRecenterGeneration();
    if (!haveReference || recenter != generation)
    {
        haveReference = true;
        generation = recenter;
        reference.yaw = boat::HeadHeading(head);
        memcpy(reference.position, headPosition, sizeof(reference.position));
        seatTurn = turretYaw;
        straightAhead = facing;
        lastTurretYaw = turretYaw;
        turnedByStickUntil = 0;
        lastSeconds = seconds;
        letGo();
        MOHW_LOG_ALWAYS(kLogFile,
                        "%s: straight ahead set. boat heading %.1f deg (%s), seat %.1f deg, head %.1f deg at "
                        "(%.2f %.2f %.2f)",
                        SeatName(seat), boatYaw / boat::kDegree, haveBoat ? "its transform" : "unreadable: no gun aim",
                        facing / boat::kDegree, reference.yaw / boat::kDegree, headPosition[0], headPosition[1],
                        headPosition[2]);
    }

    float scale = GetHeadPositionScale();
    if (!(scale > 0.01f))
        scale = 1.0f;

    float handsAt[3] = {};
    const bool paused = IsBoatPaused();
    // Not gated on the pause: a boat lying dead still would read as paused, and then nothing could turn the gun.
    const int holding = gunner && haveBoat && GetBoatGunAim() ? GrippingHands(reference, scale, handsAt) : 0;
    if (gunner)
    {
        // The thumbsticks turned it: the view comes round with it. Under the hands it doesn't.
        float moveX = 0.0f, moveY = 0.0f, turnX = 0.0f, turnY = 0.0f;
        if (holding)
            turnedByStickUntil = 0;
        else if (openvr_direct::GetVrSticks(&moveX, &moveY, &turnX, &turnY) &&
                 (fabsf(turnX) > kTurnStick || fabsf(moveX) > kTurnStick))
            turnedByStickUntil = now + kTurnedByStickMs;
        if (now < turnedByStickUntil)
            seatTurn = WrapAngleSigned(seatTurn + WrapAngleSigned(turretYaw - lastTurretYaw));
        lastTurretYaw = turretYaw;
    }

    // One heading for both eyes of an alternating pair.
    if (IsNativeStereoActive() || !IsRightEyeActive())
    {
        const float dt = static_cast<float>(seconds - lastSeconds);
        lastSeconds = seconds;
        const float a = dt > 0.0f ? 1.0f - expf(-dt / kStraightAheadSmoothingSeconds) : 0.0f;
        straightAhead = WrapAngleSigned(straightAhead + WrapAngleSigned(boatYaw + seatTurn - straightAhead) * a);
    }

    const Rigid base = boat::LevelBase(straightAhead);
    float eyePoint[3] = {game.t[0], game.t[1], game.t[2]};
    if (gunner)
    {
        float offset[3];
        // The game's eye point for a level gun: where it sits, whatever the gun's tilt when straight ahead was set.
        boat::IntoWorld(boat::kCameraFromPivot, base, offset);
        for (int k = 0; k < 3; ++k)
            eyePoint[k] = gun.t[k] + offset[k];
    }
    const Rigid camera = boat::HeadCamera(base, eyePoint, head, headPosition, reference, scale, kMaxHeadMetres);
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            view[i * 4 + k] = camera.R[i][k];
    for (int k = 0; k < 3; ++k)
        view[12 + k] = camera.t[k];

    Frame frame;
    frame.valid = true;
    frame.ms = now;
    frame.delta = Compose(camera, Inverse(FromMatrix(raw)));
    frame.quat[0] = head.x;
    frame.quat[1] = head.y;
    frame.quat[2] = head.z;
    frame.quat[3] = head.w;
    memcpy(frame.position, headPosition, sizeof(frame.position));

    float gunYaw = 0.0f, gunPitch = 0.0f, wantYaw = 0.0f, wantPitch = 0.0f;
    boat::GunAngles(gun, straightAhead, &gunYaw, &gunPitch);
    const bool levered =
        !IsFireRestricted() && lever.Update(holding, handsAt, gunYaw, gunPitch, &wantYaw, &wantPitch);
    if (levered)
    {
        const float strength = GetBoatGunAimStrength(), deadzone = GetBoatStickDeadzone();
        const float signs[2] = {yawAxis.sign, pitchAxis.sign};
        // Right is a falling heading; the turret's own answer is taken from the boat, so its turns don't count.
        frame.stick[0] = yawAxis.Update(-WrapAngleSigned(wantYaw - gunYaw), -turretYaw, strength, deadzone);
        frame.stick[1] = pitchAxis.Update(wantPitch - gunPitch, gunPitch, strength, deadzone);
        frame.haveStick = true;
        if (signs[0] != yawAxis.sign || signs[1] != pitchAxis.sign)
            MOHW_LOG_ALWAYS(kLogFile, "the turret answered the other way: stick signs now yaw %+.0f, pitch %+.0f",
                            yawAxis.sign, pitchAxis.sign);
    }
    else
    {
        letGo();
    }
    Publish(frame);

    if (now >= nextLogMs)
    {
        nextLogMs = now + 2000;
        MOHW_LOG_ALWAYS(kLogFile,
                        "%s%s: straight ahead %.1f deg, boat %.1f, seat %.1f, head moved (%.2f %.2f %.2f), raw camera %.3f m "
                        "from the view's; gun yaw %.1f pitch %.1f, hands %d want yaw %.1f pitch %.1f, stick (%.2f %.2f)%s%s",
                        SeatName(seat), paused ? " (paused)" : "", straightAhead / boat::kDegree, boatYaw / boat::kDegree,
                        facing / boat::kDegree, headPosition[0] - reference.position[0],
                        headPosition[1] - reference.position[1], headPosition[2] - reference.position[2],
                        sqrtf(dx * dx + dy * dy + dz * dz), gunYaw / boat::kDegree, gunPitch / boat::kDegree, holding,
                        wantYaw / boat::kDegree, wantPitch / boat::kDegree, frame.stick[0], frame.stick[1],
                        yawAxis.confirmed ? ", yaw proven" : "", pitchAxis.confirmed ? ", pitch proven" : "");
    }
    return true;
}

bool GetBoatHeadDelta(Rigid* delta)
{
    Frame frame;
    if (!LatestFrame(&frame))
        return false;
    *delta = frame.delta;
    return true;
}

bool GetBoatRenderPose(float quat[4], float position[3])
{
    Frame frame;
    if (!LatestFrame(&frame))
        return false;
    memcpy(quat, frame.quat, sizeof(frame.quat));
    memcpy(position, frame.position, sizeof(frame.position));
    return true;
}

bool GetBoatGunStick(float* x, float* y)
{
    Frame frame;
    if (!LatestFrame(&frame) || !frame.haveStick)
        return false;
    *x = frame.stick[0];
    *y = frame.stick[1];
    return true;
}

} // namespace mohw
