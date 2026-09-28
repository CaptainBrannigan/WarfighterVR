#include "vr_hands.h"

#include "openvr_types.h"
#include "../sdk/logging.h"
#include "../sdk/settings.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace mohw::openvr_direct {
namespace {

constexpr const char* kLogFile = "mohwvr_vrinput.log";

// ---- The game's own key bindings --------------------------------------------------------------------------------
// Documents\MOHW\settings\PROF_SAVE_profile holds lines like "GstKeyBinding.infantry.ConceptThrowGrenade.1.button 34"
// with a matching ".type" (0 = keyboard, button = DirectInput scancode, 0x80 bit = extended key; 1 = mouse). Read once,
// at the first holster use, so a key rebound in the game's own menu carries over at the next launch.
struct GameBindings
{
    bool loaded = false;
    std::map<std::string, int> scancodeByConcept; // chosen keyboard key per concept
};

std::string DocumentsFolder()
{
    using PFN_SHGetFolderPathA = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
    constexpr int kCsidlPersonal = 0x0005; // CSIDL_PERSONAL = Documents
    HMODULE shell = LoadLibraryA("shell32.dll");
    auto getFolder = shell ? reinterpret_cast<PFN_SHGetFolderPathA>(GetProcAddress(shell, "SHGetFolderPathA")) : nullptr;
    char path[MAX_PATH] = {};
    if (getFolder && SUCCEEDED(getFolder(nullptr, kCsidlPersonal, nullptr, 0, path)))
        return path;
    char profile[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::string(profile) + "\\Documents" : std::string();
}

const GameBindings& Bindings()
{
    static GameBindings bindings;
    if (bindings.loaded)
        return bindings;
    bindings.loaded = true;

    std::string path = DocumentsFolder() + "\\MOHW\\settings\\PROF_SAVE_profile";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "r") != 0 || !f)
    {
        MOHW_LOG(kLogFile, "game bindings: could not open %s -- holsters send nothing", path.c_str());
        return bindings;
    }
    // concept -> slot -> {type, button}
    std::map<std::string, std::map<int, std::pair<int, int>>> slots;
    // The infantry context, plus the singleplayer-only one (infantrySP: LTLM designator, grenade launcher).
    const char* prefixes[] = {"GstKeyBinding.infantry.", "GstKeyBinding.infantrySP."};
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        size_t prefixLen = 0;
        for (const char* p : prefixes)
            if (strncmp(line, p, strlen(p)) == 0)
                prefixLen = strlen(p);
        if (prefixLen == 0)
            continue;
        // <Concept>.<slot>.<field> <value>
        char concept[128] = {}, field[32] = {};
        int slot = -1, value = 0;
        if (sscanf_s(line + prefixLen, "%127[^.].%d.%31s %d", concept, static_cast<unsigned>(sizeof(concept)), &slot, field,
                     static_cast<unsigned>(sizeof(field)), &value) != 4)
            continue;
        auto& entry = slots[concept].emplace(slot, std::make_pair(-1, 255)).first->second;
        if (strcmp(field, "type") == 0)
            entry.first = value;
        else if (strcmp(field, "button") == 0)
            entry.second = value;
    }
    fclose(f);

    std::map<int, int> keyUseCount;
    for (auto& c : slots)
        for (auto& s : c.second)
            if (s.second.first == 0 && s.second.second != 255)
                ++keyUseCount[s.second.second];
    // Prefer a key no other action uses (melee is on both F and End, and F also toggles the weapon light).
    for (auto& c : slots)
    {
        int chosen = -1;
        for (auto& s : c.second)
            if (s.second.first == 0 && s.second.second != 255 && keyUseCount[s.second.second] == 1)
            {
                chosen = s.second.second;
                break;
            }
        if (chosen < 0)
            for (auto& s : c.second)
                if (s.second.first == 0 && s.second.second != 255)
                {
                    chosen = s.second.second;
                    break;
                }
        if (chosen >= 0)
            bindings.scancodeByConcept[c.first] = chosen;
    }
    MOHW_LOG(kLogFile, "game bindings: read %u keyboard-bound actions from %s",
              static_cast<unsigned>(bindings.scancodeByConcept.size()), path.c_str());
    return bindings;
}

int ScancodeForAction(const std::string& concept)
{
    if (concept.empty())
        return -1;
    const GameBindings& b = Bindings();
    auto it = b.scancodeByConcept.find(concept);
    return it == b.scancodeByConcept.end() ? -1 : it->second;
}

// ---- Key presses --------------------------------------------------------------------------------------------------
// Down now, up a few game ticks later: the game samples key state once per ~30 Hz tick, so a same-frame down/up can
// be missed.
struct PendingRelease
{
    int scancode;
    unsigned long long releaseAtMs;
};
std::vector<PendingRelease> g_pendingReleases; // submit thread only

void SendScancode(int dik, bool down)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = static_cast<WORD>(dik & 0x7F);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP) | ((dik & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
}

void PressKey(int dik)
{
    constexpr unsigned long long kHoldMs = 90;
    SendScancode(dik, true);
    g_pendingReleases.push_back(PendingRelease{dik, GetTickCount64() + kHoldMs});
}

void ServiceKeyReleases()
{
    unsigned long long now = GetTickCount64();
    for (size_t i = 0; i < g_pendingReleases.size();)
    {
        if (now >= g_pendingReleases[i].releaseAtMs)
        {
            SendScancode(g_pendingReleases[i].scancode, false);
            g_pendingReleases.erase(g_pendingReleases.begin() + static_cast<std::ptrdiff_t>(i));
        }
        else
        {
            ++i;
        }
    }
}

// ---- Geometry -----------------------------------------------------------------------------------------------------
Vec3 Column(const float m[3][4], int col)
{
    Vec3 v{};
    v.x = m[0][col];
    v.y = m[1][col];
    v.z = m[2][col];
    return v;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    Vec3 out{};
    out.x = a.y * b.z - a.z * b.y;
    out.y = a.z * b.x - a.x * b.z;
    out.z = a.x * b.y - a.y * b.x;
    return out;
}

bool Normalize(Vec3* v)
{
    float len = sqrtf(VecDot(*v, *v));
    if (len < 1e-4f)
        return false;
    *v = VecScale(*v, 1.0f / len);
    return true;
}

// Holster zones in the body frame: offsets from the head in meters, right / up / forward, from the HolsterXxxPos
// settings. Stomach and hip are mirrored pairs, so either hand reaches one on its own side.
struct Zone
{
    const char* name;
    int zone;      // kHolster*
    float mirror;  // +1 / -1 applied to the setting's X for the mirrored pairs, 0 = use X as is
};
const Zone kZones[] = {
    {"stomach left", kHolsterStomach, -1.0f},
    {"stomach right", kHolsterStomach, 1.0f},
    {"hip left", kHolsterHip, -1.0f},
    {"hip right", kHolsterHip, 1.0f},
    {"chest", kHolsterChest, 0.0f},
    {"left shoulder", kHolsterLeftShoulder, 0.0f},
    {"right shoulder", kHolsterRightShoulder, 0.0f},
};

// The body's facing: follows the head's yaw lazily (only past kBodyYawSlackDeg), so a glance to the side doesn't
// swing the holsters with it. Horizontal unit vector, tracking space.
constexpr float kBodyYawSlackDeg = 40.0f;
bool g_haveBody = false;
float g_bodyYaw = 0.0f; // atan2(x, z) of the horizontal body forward
Vec3 g_bodyForward{};

void UpdateBodyForward(const Quat& headOrientation)
{
    Vec3 localForward{};
    localForward.z = -1.0f;
    Vec3 f = QuatRotateVector(headOrientation, localForward);
    f.y = 0.0f;
    if (!Normalize(&f))
        return;
    float headYaw = atan2f(f.x, f.z);
    if (!g_haveBody)
    {
        g_bodyYaw = headYaw;
        g_haveBody = true;
    }
    float diff = WrapAngleSigned(headYaw - g_bodyYaw);
    float slack = kBodyYawSlackDeg * 3.14159265f / 180.0f;
    if (diff > slack)
        g_bodyYaw = WrapAngleSigned(g_bodyYaw + (diff - slack));
    else if (diff < -slack)
        g_bodyYaw = WrapAngleSigned(g_bodyYaw + (diff + slack));
    g_bodyForward = Vec3{};
    g_bodyForward.x = sinf(g_bodyYaw);
    g_bodyForward.z = cosf(g_bodyYaw);
}

// The zone the hand is in (nearest centre within HolsterRadius), or nullptr.
const Zone* FindZone(const Vec3& handPos, const float headPos[3])
{
    if (!g_haveBody)
        return nullptr;
    Vec3 up{};
    up.y = 1.0f;
    Vec3 fwd = g_bodyForward;
    Vec3 right = Cross(fwd, up); // forward x up = right for a right-handed, Y-up frame
    float radius = GetHolsterRadius();
    float drop = GetHolsterDrop();
    const Zone* best = nullptr;
    float bestDistSq = radius * radius;
    for (const Zone& z : kZones)
    {
        float offset[3];
        GetHolsterOffset(z.zone, offset);
        float r = z.mirror != 0.0f ? z.mirror * fabsf(offset[0]) : offset[0];
        Vec3 centre{};
        centre.x = headPos[0] + right.x * r + fwd.x * offset[2];
        centre.y = headPos[1] + offset[1] - drop;
        centre.z = headPos[2] + right.z * r + fwd.z * offset[2];
        Vec3 d = VecSub(handPos, centre);
        float distSq = VecDot(d, d);
        if (distSq <= bestDistSq)
        {
            bestDistSq = distSq;
            best = &z;
        }
    }
    return best;
}

// Whether the off hand is on the weapon: within TwoHandGrabRadius of the line running forward from the weapon hand,
// 5 cm to TwoHandReach along it.
bool OnBarrel(const HandInput& weapon, const HandInput& off, float* outAlong, float* outPerp)
{
    Vec3 forward = VecScale(Column(weapon.pose, 2), -1.0f);
    Vec3 toOff = VecSub(Column(off.pose, 3), Column(weapon.pose, 3));
    float along = VecDot(toOff, forward);
    Vec3 perp = VecSub(toOff, VecScale(forward, along));
    float perpDist = sqrtf(VecDot(perp, perp));
    *outAlong = along;
    *outPerp = perpDist;
    return along >= 0.05f && along <= GetTwoHandReach() && perpDist <= GetTwoHandGrabRadius();
}

// The weapon hand's orientation aimed at the off hand, keeping the weapon hand's up (so twisting it still rolls the
// gun). False if the hands are too close to give a direction.
bool AimAtOffHand(const HandInput& weapon, const HandInput& off, Quat* out)
{
    Vec3 forward = VecSub(Column(off.pose, 3), Column(weapon.pose, 3));
    if (sqrtf(VecDot(forward, forward)) < 0.08f || !Normalize(&forward))
        return false;
    Vec3 back = VecScale(forward, -1.0f);
    Vec3 up = Column(weapon.pose, 1);
    up = VecSub(up, VecScale(forward, VecDot(up, forward)));
    if (!Normalize(&up))
    {
        up = Vec3{};
        up.y = 1.0f;
        up = VecSub(up, VecScale(forward, VecDot(up, forward)));
        if (!Normalize(&up))
            return false;
    }
    Vec3 rightAxis = Cross(up, back);
    if (!Normalize(&rightAxis))
        return false;
    up = Cross(back, rightAxis);
    float m[3][4] = {};
    const Vec3* cols[3] = {&rightAxis, &up, &back};
    for (int c = 0; c < 3; ++c)
    {
        m[0][c] = cols[c]->x;
        m[1][c] = cols[c]->y;
        m[2][c] = cols[c]->z;
    }
    *out = MatrixToQuat(m);
    return true;
}

const char* HandName(int hand)
{
    return hand == kLeftHand ? "left" : "right";
}

// Submit thread state.
int g_weaponHand = kRightHand;
bool g_lastLeftHanded = false;
bool g_haveLastLeftHanded = false;
int g_grabbedBy = -1; // hand that took the weapon from a holster, for HolsterHoldToKeep
bool g_twoHanded = false;
bool g_prevGrip[2] = {false, false};
bool g_offGripIsTwoHand = false; // the off hand's current grip press is (or may become) a two-handed grip
std::atomic<bool> g_weaponHandLeft{false};

} // namespace

void UpdateHands(const HandInput hands[2], bool haveHead, const float headPos[3], const Quat& headOrientation,
                 HandsResult* out)
{
    ServiceKeyReleases();
    if (haveHead)
        UpdateBodyForward(headOrientation);

    bool leftHanded = GetLeftHanded();
    int defaultHand = leftHanded ? kLeftHand : kRightHand;
    if (!g_haveLastLeftHanded || leftHanded != g_lastLeftHanded)
    {
        g_weaponHand = defaultHand;
        g_grabbedBy = -1;
        g_twoHanded = false;
        g_haveLastLeftHanded = true;
        g_lastLeftHanded = leftHanded;
        MOHW_LOG(kLogFile, "weapon hand: %s (default, %s-handed)", HandName(g_weaponHand), leftHanded ? "left" : "right");
    }

    for (int h = 0; h < 2; ++h)
    {
        bool pressed = hands[h].grip && !g_prevGrip[h];
        bool released = !hands[h].grip && g_prevGrip[h];
        g_prevGrip[h] = hands[h].grip;
        int off = 1 - g_weaponHand;

        if (pressed && hands[h].tracked)
        {
            float along = 0.0f, perp = 0.0f;
            if (h == off && hands[g_weaponHand].tracked && OnBarrel(hands[g_weaponHand], hands[h], &along, &perp))
            {
                g_offGripIsTwoHand = true; // engaged below
            }
            else
            {
                const Zone* zone = haveHead ? FindZone(Column(hands[h].pose, 3), headPos) : nullptr;
                if (zone)
                {
                    std::string action = GetHolsterAction(zone->zone);
                    int dik = ScancodeForAction(action);
                    bool isWeapon = action.compare(0, 26, "ConceptSelectInventoryItem") == 0;
                    MOHW_LOG(kLogFile, "holster: %s hand grip in %s -> %s (key %d)%s", HandName(h), zone->name,
                              action.empty() ? "(unassigned)" : action.c_str(), dik,
                              isWeapon ? ", weapon now in this hand" : "");
                    if (dik >= 0)
                        PressKey(dik);
                    if (isWeapon && dik >= 0)
                    {
                        if (g_weaponHand != h)
                            g_twoHanded = false;
                        g_weaponHand = h;
                        g_grabbedBy = h;
                    }
                }
                else if (h == off)
                {
                    // Not on the barrel yet: sliding the hand onto it while still holding the grip engages too.
                    g_offGripIsTwoHand = true;
                    static int missLines = 0;
                    if (missLines < 20 && hands[g_weaponHand].tracked)
                    {
                        ++missLines;
                        MOHW_LOG(kLogFile,
                                  "off-hand grip not on the weapon: %.2f m along the barrel (0.05..%.2f), %.2f m off it "
                                  "(max %.2f)",
                                  along, GetTwoHandReach(), perp, GetTwoHandGrabRadius());
                    }
                }
            }
        }
        if (released)
        {
            if (h == 1 - g_weaponHand)
            {
                g_offGripIsTwoHand = false;
                if (g_twoHanded)
                {
                    g_twoHanded = false;
                    MOHW_LOG(kLogFile, "two-handed aim off");
                }
            }
            if (GetHolsterHoldToKeep() && g_grabbedBy == h && g_weaponHand == h && h != defaultHand)
            {
                g_weaponHand = defaultHand;
                g_twoHanded = false;
                MOHW_LOG(kLogFile, "weapon hand: back to %s (grip released, hold-to-keep)", HandName(g_weaponHand));
            }
            if (g_grabbedBy == h)
                g_grabbedBy = -1;
        }
    }

    int off = 1 - g_weaponHand;
    if (g_offGripIsTwoHand && hands[off].grip && !g_twoHanded && hands[off].tracked && hands[g_weaponHand].tracked)
    {
        float along = 0.0f, perp = 0.0f;
        if (OnBarrel(hands[g_weaponHand], hands[off], &along, &perp))
        {
            g_twoHanded = true;
            MOHW_LOG(kLogFile, "two-handed aim ON (%s hand on the weapon)", HandName(off));
        }
    }
    if (g_twoHanded && (!hands[off].tracked || !hands[g_weaponHand].tracked))
        g_twoHanded = false;

    out->weaponHand = g_weaponHand;
    out->twoHanded = g_twoHanded && AimAtOffHand(hands[g_weaponHand], hands[off], &out->weaponOrientation);
    out->fire = hands[g_weaponHand].trigger;
    out->ads = hands[off].trigger;
    g_weaponHandLeft.store(g_weaponHand == kLeftHand, std::memory_order_relaxed);
}

bool IsWeaponHandLeft()
{
    return g_weaponHandLeft.load(std::memory_order_relaxed);
}

// Held game keys: when each went down, so a release comes at least kMinHoldMs later. SteamVR's long-press input can
// be true for a single input update (~11 ms) and the game only samples keys once per ~30 Hz tick.
std::map<int, unsigned long long> g_gameKeyDownAt; // submit thread only

void SetGameKey(int dik, bool down)
{
    constexpr unsigned long long kMinHoldMs = 90;
    unsigned long long now = GetTickCount64();
    if (down)
    {
        SendScancode(dik, true);
        g_gameKeyDownAt[dik] = now;
        return;
    }
    auto it = g_gameKeyDownAt.find(dik);
    unsigned long long downAt = it == g_gameKeyDownAt.end() ? 0 : it->second;
    if (it != g_gameKeyDownAt.end())
        g_gameKeyDownAt.erase(it);
    if (downAt != 0 && now < downAt + kMinHoldMs)
        g_pendingReleases.push_back(PendingRelease{dik, downAt + kMinHoldMs}); // released by ServiceKeyReleases
    else
        SendScancode(dik, false);
}

int SetGameActionKey(const char* concept, bool down)
{
    int dik = ScancodeForAction(concept ? std::string(concept) : std::string());
    if (dik >= 0)
        SetGameKey(dik, down);
    return dik;
}

int SendGameScancode(int dik, bool down)
{
    if (dik >= 0)
        SetGameKey(dik, down);
    return dik;
}

void PreloadGameBindings()
{
    const GameBindings& b = Bindings();
    std::vector<std::string> choices;
    for (const auto& entry : b.scancodeByConcept)
        choices.push_back(entry.first); // std::map: already sorted by name
    SetHolsterActionChoices(choices);
    for (int zone = 0; zone < kHolsterZoneCount; ++zone)
    {
        std::string action = GetHolsterAction(zone);
        auto it = b.scancodeByConcept.find(action);
        MOHW_LOG(kLogFile, "holster zone %d -> %s -> key %d", zone, action.empty() ? "(unassigned)" : action.c_str(),
                  it == b.scancodeByConcept.end() ? -1 : it->second);
    }
}

} // namespace mohw::openvr_direct
