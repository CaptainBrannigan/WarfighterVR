#include "first_person_rig.h"

#include "aiming_controller_hook.h"
#include "../third_party/minhook/include/MinHook.h"
#include "../sdk/logging.h"
#include "../sdk/mohw_offsets.h"
#include "../sdk/settings.h"
#include "weapon_identity.h"
#include "../openvr_direct/vr_reload.h"

#include <windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_rig.log";

// Skeleton layout (see the header): the first-person character object -> its skeleton, then FUN_00683580's and
// FUN_0067BF40's fields.
constexpr uint32_t kSoldierCharacters = 0xC8;  // soldier + view mode * 4 (FUN_0098E6F0)
constexpr uint32_t kCharacterSkeleton = 0x124;
constexpr uint32_t kSkeletonBones = 0x24;      // float[12] each, the skin matrices now
constexpr uint32_t kSkeletonNames = 0x194;     // +4 count, +0x18 / +0x1C begin / end of char*[]
constexpr uint32_t kSkeletonData = 0x1BC;      // +0x1C -> +0xC -> +0x18 = bind poses, 4x4, translation in row 3
constexpr int kMaxBones = 256;

constexpr uint32_t kBatchBones = 0x50;
constexpr uint32_t kBatchBoneCount = 0x70;

// ---- SEH-safe reads ----
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

bool ReadBytes(uintptr_t address, void* out, size_t size)
{
    __try
    {
        memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadName(uintptr_t address, char* out, size_t size)
{
    if (address < 0x10000 || address >= 0xFFFF0000u)
        return false;
    __try
    {
        const char* s = reinterpret_cast<const char*>(address);
        size_t i = 0;
        for (; i + 1 < size && s[i] != '\0'; ++i)
        {
            if (s[i] < 0x20 || s[i] > 0x7E)
                return false;
            out[i] = s[i];
        }
        out[i] = '\0';
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The bones the rig's wobble against the camera is read from; the first one also corrects it. Head: it held to 0.03 deg
// against the camera, where CameraJoint swung up to 32 deg with the animations (live 2026-11-01).
const char* const kRigFrameBoneNames[3] = {"Head", "CameraJoint", "Spine2"};

// ---- The skeleton ----
struct ArmBones
{
    int upper = -1, fore = -1, hand = -1;
    std::vector<int> upperRiders; // twist bones on the upper arm
    std::vector<int> foreRiders;  // twist bones on the forearm: they take a share of the hand's roll
    std::vector<int> handBones;   // the hand + fingers
};

struct Skeleton
{
    uintptr_t object = 0; // the skeleton the rest was read from; another one = read again
    int boneCount = 0;
    std::vector<std::array<float, 3>> bind; // each bone's bind position
    ArmBones arms[2];                       // [0] left, [1] right
    bool armsValid = false;
    int magBone = -1, magAmmoBone = -1;
    int gunBone = -1;                               // Wep_Root
    int ikHand[2] = {-1, -1};                       // IK_Joint_LeftHand / IK_Joint_RightHand: where the gun holds the hands
    int aimBone = -1;                               // Wep_Aim
    int frameBones[3] = {-1, -1, -1};               // kRigFrameBoneNames
    std::vector<std::pair<int, int>> mirroredHands; // each left hand bone and its right one
    int mirrorAxis = 0;                             // the bind pose's left-right axis
    std::vector<std::string> names;                 // every bone's name ("" if unreadable)
    std::vector<int> parents;                       // every bone's parent, -1 for the root (ReadHierarchy)
    bool parentsFromGame = false;                   // read from the skeleton asset, else by name (FallbackParents)
};

std::mutex g_skeletonMutex; // ApplyArmIk and the hooks
Skeleton g_skeleton;        // guarded by g_skeletonMutex
std::atomic<uintptr_t> g_skeletonObject{0};
std::atomic<int> g_magBone{-1}, g_magAmmoBone{-1};

bool StartsWith(const char* name, const std::string& prefix)
{
    return _strnicmp(name, prefix.c_str(), prefix.size()) == 0;
}

// Sorts one bone name into an arm. The names are logged in full (mohwvr_rig.log), so a skeleton named differently shows
// there: only "RightHand" is confirmed from the game's code, the rest follow the same scheme as the engine's other
// skeletons.
void ClassifyArmBone(ArmBones& arm, const char* side, const char* name, int index)
{
    const std::string s(side);
    if (_stricmp(name, (s + "Arm").c_str()) == 0)
        arm.upper = index;
    else if (_stricmp(name, (s + "ForeArm").c_str()) == 0)
        arm.fore = index;
    else if (StartsWith(name, s + "ForeArm"))
        arm.foreRiders.push_back(index);
    else if (StartsWith(name, s + "Arm"))
        arm.upperRiders.push_back(index);
    else if (StartsWith(name, s + "Hand"))
    {
        arm.handBones.push_back(index);
        if (_stricmp(name, (s + "Hand").c_str()) == 0)
            arm.hand = index;
    }
}

// THE BONE HIERARCHY (2026-11-09, for the weapon socket view): the bind poses come from the skeleton asset
// (Animations/Skeletons/Venice1pSke01: SkeletonAsset { BoneNames, Hierarchy, LocalPose, ModelPose, ... } after the
// asset's name, so ModelPose at +0x18 and Hierarchy at +0x10, an int per bone, its parent; the count is the u32 before
// the data, as every Frostbite array). Checked before it's used: the root's parent is -1 and every other bone's an
// earlier bone, as in the game data (tools/fb2dump: Wep_Root's parent is SpineX, each socket's Wep_Root).
bool ReadHierarchy(uint32_t asset, int count, std::vector<int>* parents)
{
    uint32_t data = 0, stored = 0;
    if (!ReadU32(asset + 0x10, &data) || data < 0x10000 || !ReadU32(data - 4, &stored) ||
        stored != static_cast<uint32_t>(count))
        return false;
    std::vector<int> p(count);
    if (!ReadBytes(data, p.data(), static_cast<size_t>(count) * sizeof(int)))
        return false;
    for (int i = 0; i < count; ++i)
        if (i == 0 ? p[i] != -1 : (p[i] < 0 || p[i] >= i))
            return false;
    *parents = std::move(p);
    return true;
}

// Without the game's hierarchy: the weapon skeleton's own (WeaponSke01), by name, and each hand's weapon bone on its hand.
void FallbackParents(const std::vector<std::string>& names, std::vector<int>* parents)
{
    const int count = static_cast<int>(names.size());
    parents->assign(count, -1);
    auto find = [&](const char* name) {
        for (int i = 0; i < count; ++i)
            if (_stricmp(names[i].c_str(), name) == 0)
                return i;
        return -1;
    };
    const int root = find("Wep_Root");
    for (int i = 0; i < count; ++i)
    {
        const char* n = names[i].c_str();
        int parent = -1;
        if (_stricmp(n, "Wep_Mag_Ammo") == 0)
            parent = find("Wep_Mag");
        else if (_stricmp(n, "Wep_Grenade2") == 0)
            parent = find("Wep_Grenade1");
        else if (_stricmp(n, "Wep_Bipod2") == 0 || _stricmp(n, "Wep_Bipod3") == 0)
            parent = find("Wep_Bipod1");
        else if (StartsWith(n, "Wep_Belt") && n[8] >= '2' && n[8] <= '5' && n[9] == '\0')
        {
            char previous[16];
            snprintf(previous, sizeof(previous), "Wep_Belt%c", n[8] - 1);
            parent = find(previous);
        }
        else if (_stricmp(n, "RightWeapon") == 0)
            parent = find("RightHand");
        else if (_stricmp(n, "LeftWeapon") == 0)
            parent = find("LeftHand");
        else if (i != root && (StartsWith(n, "Wep_") || StartsWith(n, "IK_Joint_")))
            parent = root;
        (*parents)[i] = parent;
    }
}

// Reads the names and bind poses of this skeleton. Under g_skeletonMutex.
bool ReadSkeleton(uintptr_t skeleton)
{
    uint32_t namesTable = 0, count = 0, begin = 0, end = 0, data = 0, inner = 0, bindHolder = 0, bindArray = 0;
    if (!ReadU32(skeleton + kSkeletonNames, &namesTable) || !ReadU32(namesTable + 4, &count) ||
        !ReadU32(namesTable + 0x18, &begin) || !ReadU32(namesTable + 0x1C, &end) || count == 0 || count > kMaxBones ||
        end <= begin)
    {
        MOHW_LOG(kLogFile, "skeleton %p: no name table", reinterpret_cast<void*>(skeleton));
        return false;
    }
    if (!ReadU32(skeleton + kSkeletonData, &data) || !ReadU32(data + 0x1C, &inner) || !ReadU32(inner + 0xC, &bindHolder) ||
        !ReadU32(bindHolder + 0x18, &bindArray) || bindArray == 0)
    {
        MOHW_LOG(kLogFile, "skeleton %p: no bind poses (it may be the quaternion form)", reinterpret_cast<void*>(skeleton));
        return false;
    }

    Skeleton next;
    next.object = skeleton;
    next.boneCount = static_cast<int>(count);
    next.bind.resize(count);
    std::string line;
    const uint32_t named = (end - begin) / 4 < count ? (end - begin) / 4 : count;
    std::vector<std::string> names(named);
    for (uint32_t i = 0; i < named; ++i)
    {
        uint32_t namePtr = 0;
        char name[64] = "";
        if (!ReadU32(begin + i * 4, &namePtr) || !ReadName(namePtr, name, sizeof(name)))
            continue;
        float row[3];
        if (!ReadBytes(bindArray + i * 0x40 + 0x30, row, sizeof(row)))
            return false;
        next.bind[i] = {row[0], row[1], row[2]};
        names[i] = name;
        if (_stricmp(name, "Wep_Root") == 0)
            next.gunBone = static_cast<int>(i);
        for (int b = 0; b < 3; ++b)
            if (_stricmp(name, kRigFrameBoneNames[b]) == 0)
                next.frameBones[b] = static_cast<int>(i);
        ClassifyArmBone(next.arms[0], "Left", name, static_cast<int>(i));
        ClassifyArmBone(next.arms[1], "Right", name, static_cast<int>(i));
        if (_stricmp(name, "Wep_Mag") == 0)
            next.magBone = static_cast<int>(i);
        else if (_stricmp(name, "Wep_Mag_Ammo") == 0)
            next.magAmmoBone = static_cast<int>(i);
        else if (_stricmp(name, "IK_Joint_LeftHand") == 0)
            next.ikHand[0] = static_cast<int>(i);
        else if (_stricmp(name, "IK_Joint_RightHand") == 0)
            next.ikHand[1] = static_cast<int>(i);
        else if (_stricmp(name, "Wep_Aim") == 0)
            next.aimBone = static_cast<int>(i);
        char entry[96];
        snprintf(entry, sizeof(entry), " %u=%s", i, name);
        line += entry;
    }
    MOHW_LOG(kLogFile, "skeleton %p, %u bones:%s", reinterpret_cast<void*>(skeleton), count, line.c_str());

    next.names = names;
    next.names.resize(count);
    next.parentsFromGame = ReadHierarchy(bindHolder, static_cast<int>(count), &next.parents);
    if (!next.parentsFromGame)
        FallbackParents(next.names, &next.parents);
    {
        std::string weaponParents;
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::string& n = next.names[i];
            if (StartsWith(n.c_str(), "Wep_") || StartsWith(n.c_str(), "IK_Joint_") || _stricmp(n.c_str(), "RightWeapon") == 0 ||
                _stricmp(n.c_str(), "LeftWeapon") == 0)
            {
                const int p = next.parents[i];
                char entry[96];
                snprintf(entry, sizeof(entry), " %s<-%s", n.c_str(), p >= 0 ? next.names[p].c_str() : "(none)");
                weaponParents += entry;
            }
        }
        MOHW_LOG(kLogFile, "bone hierarchy %s; the weapon bones' parents:%s",
                 next.parentsFromGame ? "read from the skeleton asset" : "NOT readable, taken by name", weaponParents.c_str());
    }

    // For the gun in the left hand: each left hand bone's right twin, and the axis the two sides mirror across.
    for (uint32_t i = 0; i < named; ++i)
        if (StartsWith(names[i].c_str(), "LeftHand"))
            for (uint32_t k = 0; k < named; ++k)
                if (_stricmp(names[k].c_str(), ("Right" + names[i].substr(4)).c_str()) == 0)
                    next.mirroredHands.push_back({static_cast<int>(i), static_cast<int>(k)});
    next.armsValid = true;
    for (int h = 0; h < 2; ++h)
    {
        const ArmBones& a = next.arms[h];
        if (a.upper < 0 || a.fore < 0 || a.hand < 0)
            next.armsValid = false;
        MOHW_LOG(kLogFile, "%s arm: upper %d (%u twist), forearm %d (%u twist), hand %d (%u with the fingers)",
                 h ? "right" : "left", a.upper, static_cast<unsigned>(a.upperRiders.size()), a.fore,
                 static_cast<unsigned>(a.foreRiders.size()), a.hand, static_cast<unsigned>(a.handBones.size()));
    }
    if (next.armsValid)
    {
        float widest = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float apart = std::fabs(next.bind[next.arms[0].hand][k] - next.bind[next.arms[1].hand][k]);
            if (apart > widest)
            {
                widest = apart;
                next.mirrorAxis = k;
            }
        }
    }
    MOHW_LOG(kLogFile, "gun in the left hand: %u mirrored hand bones across bind axis %d, gun bone %d",
             static_cast<unsigned>(next.mirroredHands.size()), next.mirrorAxis, next.gunBone);
    MOHW_LOG(kLogFile, "magazine bones: Wep_Mag %d, Wep_Mag_Ammo %d; arm IK %s", next.magBone, next.magAmmoBone,
             next.armsValid ? "ready" : "NOT available: arm bones missing, see the names above");
    MOHW_LOG(kLogFile, "hand sockets: IK_Joint_LeftHand %d, IK_Joint_RightHand %d, Wep_Aim %d", next.ikHand[0],
             next.ikHand[1], next.aimBone);
    g_magBone.store(next.magBone, std::memory_order_relaxed);
    g_magAmmoBone.store(next.magAmmoBone, std::memory_order_relaxed);
    g_skeleton = std::move(next);
    return true;
}

// ---- Hooks ----
// FUN_009551F0: the bones of a soldier's current character. Hooked only to collect (skeleton, bones) candidates: which
// one is the first-person rig is settled by content, NoteBodyBatch, as the body batch holds a copy of those bones.
// (2026-11-01: this first filtered on the view mode, and live that matched no soldier at all.)
using BonesGetterFn = void*(__fastcall*)(void* soldier);
BonesGetterFn g_originalBonesGetter = nullptr;
void* g_bonesGetterAddress = nullptr;

struct SkeletonCandidate
{
    uintptr_t bones;
    uintptr_t skeleton;
};
constexpr int kMaxCandidates = 48;
std::mutex g_candidateMutex;
SkeletonCandidate g_candidates[kMaxCandidates]; // guarded by g_candidateMutex
int g_candidateCount = 0, g_candidateNext = 0;
std::atomic<bool> g_wantCandidates{true}; // only while no skeleton is settled
// The player's bones (as the game returns them), and the frame bones' rotations from them this job, for AlignRigToBones:
// read here because this is the copy the batches are made from, and a later read could be a tick newer.
std::atomic<uintptr_t> g_playerBones{0};
std::atomic<int> g_frameBoneIndex[3] = {{-1}, {-1}, {-1}};
std::mutex g_frameMutex;
float g_frameRows[3][3][3] = {}; // guarded by g_frameMutex
bool g_haveFrameRows = false;    // likewise

void NoteCandidate(uintptr_t bones, uintptr_t skeleton)
{
    if (!bones || !skeleton)
        return;
    std::lock_guard<std::mutex> lock(g_candidateMutex);
    for (int i = 0; i < g_candidateCount; ++i)
        if (g_candidates[i].bones == bones && g_candidates[i].skeleton == skeleton)
            return;
    g_candidates[g_candidateNext] = SkeletonCandidate{bones, skeleton};
    g_candidateNext = (g_candidateNext + 1) % kMaxCandidates;
    if (g_candidateCount < kMaxCandidates)
        ++g_candidateCount;
}

// SEH-safe: the character objects' skeletons, the view modes' (+0xC8, +0xCC) and the current one by the game's virtual.
void CollectCandidates(void* soldier, void* bones)
{
    uintptr_t skeletons[3] = {};
    uintptr_t arrays[3] = {};
    uintptr_t current = 0;
    __try
    {
        uintptr_t base = reinterpret_cast<uintptr_t>(soldier);
        for (int mode = 0; mode < 2; ++mode)
        {
            uintptr_t character = *reinterpret_cast<const uintptr_t*>(base + kSoldierCharacters + mode * 4);
            if (!character)
                continue;
            skeletons[mode] = *reinterpret_cast<const uintptr_t*>(character + kCharacterSkeleton);
            if (skeletons[mode])
                arrays[mode] = *reinterpret_cast<const uintptr_t*>(skeletons[mode] + kSkeletonBones);
        }
        using CharacterFn = uintptr_t(__thiscall*)(void*);
        auto characterOf = (*reinterpret_cast<CharacterFn**>(soldier))[0x174 / 4];
        uintptr_t character = characterOf(soldier);
        if (character)
            current = *reinterpret_cast<const uintptr_t*>(character + kCharacterSkeleton);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    for (int i = 0; i < 2; ++i)
        NoteCandidate(arrays[i], skeletons[i]);
    NoteCandidate(reinterpret_cast<uintptr_t>(bones), current);
}

void NoteFrameBones(uintptr_t bones)
{
    float rows[3][3][3] = {};
    for (int b = 0; b < 3; ++b)
    {
        const int index = g_frameBoneIndex[b].load(std::memory_order_relaxed);
        float slot[12];
        if (index < 0 || !ReadBytes(bones + index * 48, slot, sizeof(slot)))
            continue;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rows[b][i][j] = slot[j * 4 + i]; // a bone is the transpose of the row-vector matrix
    }
    std::lock_guard<std::mutex> lock(g_frameMutex);
    memcpy(g_frameRows, rows, sizeof(rows));
    g_haveFrameRows = true;
}

void* __fastcall HookedBonesGetter(void* soldier)
{
    void* bones = g_originalBonesGetter(soldier);
    if (g_wantCandidates.load(std::memory_order_relaxed))
        CollectCandidates(soldier, bones);
    else if (reinterpret_cast<uintptr_t>(bones) == g_playerBones.load(std::memory_order_relaxed))
        NoteFrameBones(reinterpret_cast<uintptr_t>(bones));
    return bones;
}

// FUN_00A37160: places one weapon part. The magazine's meshes, noted by the bone they're placed on.
constexpr int kMaxMagazineMeshes = 8;
std::atomic<uintptr_t> g_magazineMeshes[kMaxMagazineMeshes];
std::atomic<unsigned> g_magazineGeneration{0};
constexpr uint32_t kRigidRecordBytes = 0x50;

void NoteMagazineMesh(uintptr_t mesh)
{
    for (auto& slot : g_magazineMeshes)
    {
        uintptr_t held = slot.load(std::memory_order_relaxed);
        if (held == mesh)
            return;
        if (held == 0)
        {
            slot.store(mesh, std::memory_order_relaxed);
            MOHW_LOG(kLogFile, "magazine part: mesh %p", reinterpret_cast<void*>(mesh));
            return;
        }
    }
}

using PlacePartFn = void(__thiscall*)(void* entry, int firstPerson, int a2, int a3, void* character, void* rigidList,
                                      int a6, int a7, int weaponBones, int hiddenBone, int a10);
PlacePartFn g_originalPlacePart = nullptr;
void* g_placePartAddress = nullptr;

// SEH-safe: the rigid list's record count, and the mesh of record i.
bool ReadRigidList(void* list, uint32_t* begin, uint32_t* end)
{
    return ReadU32(reinterpret_cast<uintptr_t>(list), begin) && ReadU32(reinterpret_cast<uintptr_t>(list) + 4, end);
}

void __fastcall HookedPlacePart(void* entry, void* /*edx*/, int firstPerson, int a2, int a3, void* character,
                                void* rigidList, int a6, int a7, int weaponBones, int hiddenBone, int a10)
{
    uint32_t begin = 0, end = 0;
    bool haveBefore = rigidList && ReadRigidList(rigidList, &begin, &end);
    uint32_t before = haveBefore ? (end - begin) / kRigidRecordBytes : 0;
    g_originalPlacePart(entry, firstPerson, a2, a3, character, rigidList, a6, a7, weaponBones, hiddenBone, a10);
    if (!haveBefore || (firstPerson & 0xFF) == 0 || (a10 & 0xFF) != 0)
        return;
    int magBone = g_magBone.load(std::memory_order_relaxed);
    int ammoBone = g_magAmmoBone.load(std::memory_order_relaxed);
    if (magBone < 0)
        return;
    uint32_t bone = 0;
    if (!ReadU32(reinterpret_cast<uintptr_t>(entry), &bone) ||
        (static_cast<int>(bone) != magBone && static_cast<int>(bone) != ammoBone))
        return;
    if (!ReadRigidList(rigidList, &begin, &end))
        return;
    uint32_t after = (end - begin) / kRigidRecordBytes;
    for (uint32_t i = before; i < after; ++i)
    {
        uint32_t mesh = 0;
        if (ReadU32(begin + i * kRigidRecordBytes, &mesh) && mesh)
            NoteMagazineMesh(mesh);
    }
}

// FUN_00831080: the reload's time, for the firing state machine's timers. None for manual reload: the hand has already
// done it, and at no time a round-at-a-time reload runs to full within one tick. Hand-written, as the game is compiled
// knowing this function leaves ECX alone: FUN_0083B130 calls it and then FUN_00831170 with no ECX of its own, so a C++
// detour, free to use ECX, crashed there (live 2026-11-01, fault at 0x00831170). This touches only EAX, which the original
// overwrites first thing. Only FUN_0083B130's own calls: FUN_00835620 divides by the time.
// The same code also runs on the server's copy of the firing (ticked by FUN_00BB3C90), which is no object of the client's
// and goes by the firing data (+0x94) the two share, for the moment around a seat only: left at full length it finished
// seconds after the magazine went in.
uintptr_t g_reloadTimeCallersFirst = 0;
volatile uintptr_t g_instantReloadFiring = 0;
volatile uintptr_t g_instantReloadData = 0;
volatile uintptr_t g_otherReloadFiring = 0;
void* g_originalReloadTime = nullptr;
void* g_reloadTimeAddress = nullptr;

__declspec(naked) void ReloadTimeStub()
{
    __asm {
        mov eax, dword ptr [esp]
        sub eax, g_reloadTimeCallersFirst
        cmp eax, 0xCA6 // FUN_0083B130's size
        jae original
        cmp ecx, g_instantReloadFiring
        je instant
        mov eax, dword ptr [ecx + 0x94]
        cmp eax, g_instantReloadData
        jne original
        mov g_otherReloadFiring, ecx
    instant:
        fldz
        ret
    original:
        jmp g_originalReloadTime
    }
}

// FUN_008373E0: a reload's start, announced to the animation. Manual reload's isn't: the hand has already done it.
using ReloadStartedFn = void(__thiscall*)(void* firing, int local);
ReloadStartedFn g_originalReloadStarted = nullptr;
void* g_reloadStartedAddress = nullptr;

void __fastcall HookedReloadStarted(void* firing, void* /*edx*/, int local)
{
    if (openvr_direct::ManualReloadCancelsAnimation(firing))
    {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 8)
            MOHW_LOG(kLogFile, "reload start not announced: no animation");
        return;
    }
    g_originalReloadStarted(firing, local);
}

// ---- Matrices (the AvP port's convention: row-major 4x4, v' = v * S, rows 0-2 = axes, row 3 = translation) ----
void Mul4(const float a[16], const float b[16], float c[16])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            c[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
}

// A bone = the transpose of S: 3 rows of 4, translation last.
float* BoneSlot(float* bones, int bone)
{
    return bones + bone * 12;
}

void ReadBone(const float* b, float s[16])
{
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 4; ++row)
            s[row * 4 + col] = b[col * 4 + row];
    s[3] = s[7] = s[11] = 0.0f;
    s[15] = 1.0f;
}

void WriteBone(float* b, const float s[16])
{
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 4; ++row)
            b[col * 4 + row] = s[row * 4 + col];
}

void MoveBone(float* bones, int bone, const float m[16])
{
    float s[16], ns[16];
    float* slot = BoneSlot(bones, bone);
    ReadBone(slot, s);
    Mul4(s, m, ns);
    WriteBone(slot, ns);
}

void ToMatrix4(const Rigid& a, float m[16])
{
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = a.R[i][j];
        m[i * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; ++j)
        m[12 + j] = a.t[j];
    m[15] = 1.0f;
}

void Sub3(const float a[3], const float b[3], float o[3])
{
    for (int k = 0; k < 3; ++k)
        o[k] = a[k] - b[k];
}
float Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float Len3(const float a[3])
{
    return std::sqrt(Dot3(a, a));
}
void Cross3(const float a[3], const float b[3], float o[3])
{
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
bool Normalize3(float a[3])
{
    const float l = Len3(a);
    if (l < 1e-6f)
        return false;
    for (int k = 0; k < 3; ++k)
        a[k] /= l;
    return true;
}
// v minus its part along the unit axis, normalised.
bool PerpTo(const float axis[3], const float v[3], float o[3])
{
    const float dn = Dot3(v, axis);
    for (int k = 0; k < 3; ++k)
        o[k] = v[k] - dn * axis[k];
    return Normalize3(o);
}
void PointThrough(const float p[3], const float m[16], float o[3])
{
    float r[3];
    for (int j = 0; j < 3; ++j)
        r[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
    o[0] = r[0], o[1] = r[1], o[2] = r[2];
}
void VecTimes(const float v[3], const float r[9], float o[3]) // v * R (row vector, 3x3)
{
    float t[3];
    for (int j = 0; j < 3; ++j)
        t[j] = v[0] * r[0 * 3 + j] + v[1] * r[1 * 3 + j] + v[2] * r[2 * 3 + j];
    o[0] = t[0], o[1] = t[1], o[2] = t[2];
}
// The rotation (row vectors) taking the orthonormal frame (x, z x x, z) onto (x2, z2 x x2, z2): A^T * A2.
void FrameToFrame(const float x[3], const float z[3], const float x2[3], const float z2[3], float r[9])
{
    float y[3], y2[3];
    Cross3(z, x, y);
    Cross3(z2, x2, y2);
    const float* a[3] = {x, y, z};
    const float* b[3] = {x2, y2, z2};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[0][i] * b[0][j] + a[1][i] * b[1][j] + a[2][i] * b[2][j];
}
// Rotation (row vectors) by angle about the unit axis, right-handed.
void AxisRotation(const float a[3], float angle, float r[9])
{
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float col[9] = {c + a[0] * a[0] * t,        a[0] * a[1] * t - a[2] * s, a[0] * a[2] * t + a[1] * s,
                          a[1] * a[0] * t + a[2] * s, c + a[1] * a[1] * t,        a[1] * a[2] * t - a[0] * s,
                          a[2] * a[0] * t - a[1] * s, a[2] * a[1] * t + a[0] * s, c + a[2] * a[2] * t};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = col[j * 3 + i];
}
// The move v' = (v - from) * R + to.
void PivotMove(const float r[9], const float from[3], const float to[3], float m[16])
{
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = r[i * 3 + j];
        m[i * 4 + 3] = 0.0f;
    }
    float f[3];
    VecTimes(from, r, f);
    for (int j = 0; j < 3; ++j)
        m[12 + j] = to[j] - f[j];
    m[15] = 1.0f;
}

// ---- The natural elbow model, from AvP (2026-10-21) ----
// The elbow is one angle about the shoulder-wrist line, measured from a zero that can't become undefined: the body's
// down, blended toward its back as the arm comes near vertical (arms hanging: elbows back). The wrist's roll is the
// hand's own: the controller's down side about its pointing axis, from the body's down tilted with the hand's pitch, so
// it depends only on how the hand is held, bent arm or straight. + = outward, unwrapped against the last frame. The angle
// = kFollow x roll, through a soft-knee curve per side. Only the inward knee and max shrink, to kInNearBody, as the hand
// comes in toward the body. The angles are settings (ArmIkSettings), defaulting to AvP's Marine values.
constexpr float kPi = 3.14159265f;
constexpr float kRad = 0.01745329252f;
constexpr float kNearBodyM = 0.15f, kAwayFromBodyM = 0.45f;
constexpr DWORD kElbowGapMs = 500;  // no update for this long: start afresh
constexpr float kSenseClear = 0.3f; // a sense is only taken when the side lies this clearly one way
constexpr float kSenseFlip = 0.5f;  // and only flips when it lies this clearly the other way

struct ElbowState
{
    bool have = false;
    float rawRoll = 0.0f, roll = 0.0f, theta = 0.0f;
    float rollSense = 1.0f, outSense = 1.0f;
    DWORD tick = 0;
};
ElbowState g_elbow[2];

float SmoothStep(float e0, float e1, float x)
{
    const float t = std::fmin(1.0f, std::fmax(0.0f, (x - e0) / (e1 - e0)));
    return t * t * (3.0f - 2.0f * t);
}
// x >= 0: x up to the knee, then easing toward max (slope 1 at the knee, never reaching max).
float SoftKnee(float x, float knee, float max)
{
    if (x <= knee)
        return x;
    if (max <= knee)
        return knee;
    const float span = max - knee;
    return knee + span * std::tanh((x - knee) / span);
}
float WrapPi(float a)
{
    while (a > kPi)
        a -= 2.0f * kPi;
    while (a <= -kPi)
        a += 2.0f * kPi;
    return a;
}
// The body's down made perpendicular to axis, tilted toward the body's back as axis nears vertical.
bool BodyDownAbout(const float axis[3], const float forward[3], float out[3])
{
    const float down[3] = {0.0f, -1.0f, 0.0f};
    const float ad = Dot3(axis, down);
    const float offVertical = std::sqrt(std::fmax(0.0f, 1.0f - ad * ad));
    const float w = 1.0f - SmoothStep(0.2f, 0.5f, offVertical);
    const float backSide = ad >= 0.0f ? 1.0f : -1.0f;
    float v[3];
    for (int k = 0; k < 3; ++k)
        v[k] = -forward[k] * w * backSide + down[k] * (1.0f - w);
    return PerpTo(axis, v, out);
}
void UpdateSense(const float axis[3], const float from[3], const float out[3], float& sense, bool decided)
{
    float t[3];
    Cross3(axis, from, t);
    if (!Normalize3(t))
        return;
    const float along = Dot3(t, out);
    const float wanted = along >= 0.0f ? 1.0f : -1.0f;
    if (!decided)
    {
        if (std::fabs(along) > kSenseClear)
            sense = wanted;
    }
    else if (wanted != sense && std::fabs(along) > kSenseFlip)
        sense = wanted;
}

// The elbow's side (unit, perpendicular to n) from the model. controllerForward / controllerDown: the hand, rig space.
bool ElbowFromModel(const ArmIkSettings& m, int hand, const float n[3], const float forward[3], const float right[3],
                    const float controllerForward[3], const float controllerDown[3], float bodyDist, float side[3])
{
    ElbowState& s = g_elbow[hand];
    const DWORD now = GetTickCount();
    if (s.have && now - s.tick > kElbowGapMs)
        s.have = false;

    float zero[3];
    if (!BodyDownAbout(n, forward, zero))
        return false;
    const float sign = hand ? 1.0f : -1.0f;
    const float out[3] = {sign * right[0], sign * right[1], sign * right[2]};
    UpdateSense(n, zero, out, s.outSense, s.have);

    float f[3] = {controllerForward[0], controllerForward[1], controllerForward[2]};
    float zf[3], dP[3], cr[3];
    bool reliable = false;
    if (Normalize3(f) && BodyDownAbout(f, forward, zf) && PerpTo(f, controllerDown, dP))
    {
        UpdateSense(f, zf, out, s.rollSense, s.have);
        Cross3(zf, dP, cr);
        const float raw = std::atan2(Dot3(cr, f), Dot3(zf, dP));
        if (!s.have)
            s.rawRoll = s.roll = raw;
        else
        {
            s.roll += WrapPi(raw - s.rawRoll);
            s.rawRoll = raw;
            if (s.roll > 1.5f * kPi || s.roll < -1.5f * kPi)
                s.roll = WrapPi(s.roll);
        }
        reliable = true;
    }
    if (!reliable && !s.have)
        return false;

    const float outward = m.follow * s.roll * s.rollSense;
    float shaped;
    if (outward >= 0.0f)
        shaped = SoftKnee(outward, m.outKneeDeg * kRad, m.outMaxDeg * kRad);
    else
    {
        const float scale = m.inNearBody + (1.0f - m.inNearBody) * SmoothStep(kNearBodyM, kAwayFromBodyM, bodyDist);
        shaped = -SoftKnee(-outward, m.inKneeDeg * kRad * scale, m.inMaxDeg * kRad * scale);
    }
    if (!s.have || m.smoothingMs <= 0.0f)
        s.theta = shaped;
    else
        s.theta += (shaped - s.theta) * (1.0f - std::exp(-static_cast<float>(now - s.tick) / m.smoothingMs));
    s.have = true;
    s.tick = now;
    float rs[9];
    AxisRotation(n, s.theta * s.outSense, rs);
    VecTimes(zero, rs, side);
    return true;
}

bool g_armLogged[2] = {};

// The hand's centre: its bones' bind positions under their current matrices. Rig space.
bool HandCentre(float* bones, const Skeleton& sk, const ArmBones& a, float p[3])
{
    p[0] = p[1] = p[2] = 0.0f;
    int n = 0;
    for (int b : a.handBones)
    {
        float s[16], c[3];
        ReadBone(BoneSlot(bones, b), s);
        PointThrough(sk.bind[b].data(), s, c);
        for (int k = 0; k < 3; ++k)
            p[k] += c[k];
        ++n;
    }
    if (n == 0)
        return false;
    for (int k = 0; k < 3; ++k)
        p[k] /= static_cast<float>(n);
    return true;
}

// One arm: the hand's bones take tm (rig space), then two-bone IK: the shoulder stays, the upper arm and forearm turn
// about the shoulder and elbow so the wrist meets the hand. As AvP's ArmToController, without the vertex-derived joints:
// the bind poses give them directly. Past full reach the arm is straight and the shoulder follows the hand.
void SolveArm(const ArmIkSettings& settings, float* bones, const Skeleton& sk, int hand, const float tm[16],
              const float forward[3], const float right[3], const float head[3], const float controllerForward[3],
              const float controllerDown[3], bool twist)
{
    const ArmBones& r = sk.arms[hand];
    float su[16], sf[16], sh[16];
    ReadBone(BoneSlot(bones, r.upper), su);
    ReadBone(BoneSlot(bones, r.fore), sf);
    ReadBone(BoneSlot(bones, r.hand), sh);
    for (int b : r.handBones)
        MoveBone(bones, b, tm);

    float shoulder[3], elbowU[3], elbow[3], wristF[3], wrist[3];
    PointThrough(sk.bind[r.upper].data(), su, shoulder);
    PointThrough(sk.bind[r.fore].data(), su, elbowU);
    PointThrough(sk.bind[r.fore].data(), sf, elbow);
    PointThrough(sk.bind[r.hand].data(), sf, wristF);
    PointThrough(sk.bind[r.hand].data(), sh, wrist);
    float upperV[3], foreV[3];
    Sub3(elbowU, shoulder, upperV);
    Sub3(wristF, elbow, foreV);
    const float lu = Len3(upperV), lf = Len3(foreV);
    const bool firstLog = !g_armLogged[hand];
    g_armLogged[hand] = true;
    auto rigid = [&](const char* why) {
        for (int b : {r.upper, r.fore})
            MoveBone(bones, b, tm);
        for (int b : r.upperRiders)
            MoveBone(bones, b, tm);
        for (int b : r.foreRiders)
            MoveBone(bones, b, tm);
        if (firstLog)
            MOHW_LOG(kLogFile, "%s arm moved rigidly (%s)", hand ? "right" : "left", why);
    };
    if (lu < 1e-3f || lf < 1e-3f)
        return rigid("degenerate joints");

    float wrist2[3], handRot[9];
    PointThrough(wrist, tm, wrist2);
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            handRot[i * 3 + k] = tm[i * 4 + k];

    // This shoulder, moved by its setting: outward (away from the body, on either side), up, forward.
    const float* shoulderOffset = settings.shoulder[hand];
    const float outward = (hand ? 1.0f : -1.0f) * shoulderOffset[0];
    const float worldUp[3] = {0.0f, 1.0f, 0.0f};
    float shoulder2[3], n[3];
    for (int k = 0; k < 3; ++k)
        shoulder2[k] =
            shoulder[k] + right[k] * outward + worldUp[k] * shoulderOffset[1] + forward[k] * shoulderOffset[2];
    Sub3(wrist2, shoulder2, n);
    const float wanted = Len3(n);
    if (!Normalize3(n))
        return rigid("hand on the shoulder");
    const float reachMax = (lu + lf) * 0.999f, reachMin = std::fabs(lu - lf) * 1.001f + 1e-3f;
    float dist = wanted;
    if (dist > reachMax || dist < reachMin)
    {
        dist = dist > reachMax ? reachMax : reachMin;
        for (int k = 0; k < 3; ++k)
            shoulder2[k] = wrist2[k] - n[k] * dist;
    }
    const float x = (lu * lu - lf * lf + dist * dist) / (2.0f * dist);
    const float radius = std::sqrt(std::fmax(0.0f, lu * lu - x * x));

    // The elbow's side: the natural model, else toward where the forearm would continue straight back from the hand.
    const float foreDir[3] = {foreV[0] / lf, foreV[1] / lf, foreV[2] / lf};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    float side[3];
    const float bx = wrist2[0] - head[0], bz = wrist2[2] - head[2];
    if (!settings.naturalElbow || !ElbowFromModel(settings, hand, n, forward, right, controllerForward, controllerDown,
                                                  std::sqrt(bx * bx + bz * bz), side))
    {
        float foreDir2[3], hint[3];
        VecTimes(foreDir, handRot, foreDir2);
        for (int k = 0; k < 3; ++k)
            hint[k] = wrist2[k] - lf * foreDir2[k] - shoulder2[k];
        if (!PerpTo(n, hint, side) && !PerpTo(n, upperV, side) && !PerpTo(n, down, side))
            return rigid("no elbow direction");
        if (settings.elbowSwivelDeg != 0.0f)
        {
            float rs[9];
            AxisRotation(n, settings.elbowSwivelDeg * kRad * (hand ? 1.0f : -1.0f), rs);
            VecTimes(side, rs, side);
        }
    }
    float elbow2[3];
    for (int k = 0; k < 3; ++k)
        elbow2[k] = shoulder2[k] + n[k] * x + side[k] * radius;

    // Hinge frames: axis along each segment, the bend normal (upper x fore), old and new.
    float ux[3] = {upperV[0] / lu, upperV[1] / lu, upperV[2] / lu}, ux2[3], fx2[3], bend[3], bend2[3];
    Sub3(elbow2, shoulder2, ux2);
    Sub3(wrist2, elbow2, fx2);
    if (!Normalize3(ux2) || !Normalize3(fx2))
        return rigid("degenerate solve");
    Cross3(upperV, foreV, bend);
    if (!Normalize3(bend))
    {
        Cross3(ux, down, bend); // the game's arm is straight: any plane through it
        if (!Normalize3(bend))
            return rigid("straight arm along the vertical");
    }
    Cross3(ux2, fx2, bend2);
    if (!Normalize3(bend2))
        Cross3(side, n, bend2); // straight after the solve: the plane of the elbow's side
    float zu[3], zf[3], zu2[3], zf2[3];
    if (!PerpTo(ux, bend, zu) || !PerpTo(foreDir, bend, zf) || !PerpTo(ux2, bend2, zu2) || !PerpTo(fx2, bend2, zf2))
        return rigid("degenerate frames");
    float ru[9], rf[9], mu[16], mf[16];
    FrameToFrame(ux, zu, ux2, zu2, ru);
    FrameToFrame(foreDir, zf, fx2, zf2, rf);
    PivotMove(ru, shoulder, shoulder2, mu);
    PivotMove(rf, elbow, elbow2, mf);
    MoveBone(bones, r.upper, mu);
    for (int b : r.upperRiders)
        MoveBone(bones, b, mu);
    MoveBone(bones, r.fore, mf);

    // The forearm's twist bones: a share of the hand's roll about the new forearm.
    float zfHand[3], proj[3], cr[3];
    VecTimes(zf, handRot, zfHand);
    float roll = 0.0f;
    if (twist && PerpTo(fx2, zfHand, proj))
    {
        Cross3(zf2, proj, cr);
        roll = std::atan2(Dot3(cr, fx2), Dot3(zf2, proj));
        float rt[9], tw[16], mt[16];
        AxisRotation(fx2, roll * settings.forearmTwist, rt);
        PivotMove(rt, elbow2, elbow2, tw);
        Mul4(mf, tw, mt);
        for (int b : r.foreRiders)
            MoveBone(bones, b, mt);
    }
    else
    {
        for (int b : r.foreRiders)
            MoveBone(bones, b, mf);
    }
    if (firstLog)
        MOHW_LOG(kLogFile, "%s arm IK: upper arm %.3f, forearm %.3f, shoulder to hand %.3f (%s), hand roll %.0f deg",
                 hand ? "right" : "left", lu, lf, wanted, wanted > reachMax ? "past full reach" : "within reach",
                 roll / kRad);
}

// Rotation = roll (about back), then pitch (about right), then yaw (about up), as the gun's grip.
Rigid AnglesRotation(const float deg[3])
{
    float cp = cosf(deg[0] * kRad), sp = sinf(deg[0] * kRad);
    float cy = cosf(deg[1] * kRad), sy = sinf(deg[1] * kRad);
    float cr = cosf(deg[2] * kRad), sr = sinf(deg[2] * kRad);
    Rigid roll{{{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}}, {0, 0, 0}};
    Rigid pitch{{{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}}, {0, 0, 0}};
    Rigid yaw{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}, {0, 0, 0}};
    return Compose(Compose(roll, pitch), yaw);
}

// A hand's calibration on its controller: where the controller holds it, as the gun's grip does for the gun. The hand's
// centre lands on the controller, moved by position (right, up, forward) in the controller's frame, the hand turned by
// rotation (pitch, yaw, roll); both mirrored with the gun in the left hand.
Rigid HandOnController(const float position[3], const float rotationDeg[3], const float centreInCamera[3], bool mirrored)
{
    float deg[3] = {rotationDeg[0], rotationDeg[1], rotationDeg[2]};
    float off[3] = {position[0], position[1], -position[2]};
    if (mirrored)
    {
        off[0] = -off[0];
        deg[1] = -deg[1];
        deg[2] = -deg[2];
    }
    Rigid j = AnglesRotation(deg);
    for (int k = 0; k < 3; ++k)
        j.t[k] = centreInCamera[k] - (off[0] * j.R[0][k] + off[1] * j.R[1][k] + off[2] * j.R[2][k]);
    return j;
}

// The off hand's: LeftHandIk*.
Rigid LeftHandGrip(const float centreInCamera[3], bool mirrored)
{
    float position[3], deg[3];
    GetLeftHandIkOffset(position, deg);
    return HandOnController(position, deg, centreInCamera, mirrored);
}

// The weapon hand's: RightHandIk* (2026-11-09). It used to ride the gun (the gun's move, plus this as an offset on the
// gun), so it went wherever a grip profile put the gun; now it's on its own controller like the off hand, and the gun
// follows the hand instead ("Gun held at its hand socket").
Rigid WeaponHandGrip(const ArmIkSettings& s, const float centreInCamera[3], bool mirrored)
{
    return HandOnController(s.rightHandPos, s.rightHandDeg, centreInCamera, mirrored);
}

// THE GUN IN THE LEFT HAND (2026-11-01): the rig's right hand is the one animated on the grip, and its left on the
// handguard, so the two hands trade poses, each mirrored across the gun's vertical plane in the rest view. A vertex of
// the left hand mirrored in the bind pose (X) is the matching vertex of the right hand, so the new left bone is
// X * right bone * W, W being the mirror across the gun's plane: two mirrors, so a proper rigid matrix again.
void MirrorHands(float* bones, const Skeleton& sk, const Rigid& cam, const Rigid& camInverse)
{
    float gunInCamera = 0.0f;
    if (sk.gunBone >= 0 && sk.gunBone < static_cast<int>(sk.bind.size()))
    {
        float s[16], point[3], inCamera[3];
        ReadBone(BoneSlot(bones, sk.gunBone), s);
        PointThrough(sk.bind[sk.gunBone].data(), s, point);
        PointTimes(point, camInverse, inCamera);
        gunInCamera = inCamera[0];
    }
    float x[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    x[sk.mirrorAxis * 5] = -1.0f;
    const float acrossGun[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2.0f * gunInCamera, 0, 0, 1};
    float toCamera[16], fromCamera[16], w1[16], w[16];
    ToMatrix4(camInverse, toCamera);
    ToMatrix4(cam, fromCamera);
    Mul4(toCamera, acrossGun, w1);
    Mul4(w1, fromCamera, w);
    for (const auto& pair : sk.mirroredHands)
    {
        float left[16], right[16], step[16], newLeft[16], newRight[16];
        ReadBone(BoneSlot(bones, pair.first), left);
        ReadBone(BoneSlot(bones, pair.second), right);
        Mul4(x, right, step);
        Mul4(step, w, newLeft);
        Mul4(x, left, step);
        Mul4(step, w, newRight);
        WriteBone(BoneSlot(bones, pair.first), newLeft);
        WriteBone(BoneSlot(bones, pair.second), newRight);
    }
}

// Which meshes are skinned to the arms, by their sections' bone lists (FUN_00723800's layout), remembered per mesh.
struct MeshClass
{
    uintptr_t mesh;
    bool arm;
};
constexpr int kMaxMeshClasses = 32;
MeshClass g_meshClasses[kMaxMeshClasses];
int g_meshClassCount = 0;
std::mutex g_meshClassMutex;
std::atomic<DWORD> g_armsDrawnTick{0};

// SEH-safe: whether any section of the mesh uses one of the bones.
bool MeshUsesBones(uintptr_t mesh, const int* wanted, int wantedCount)
{
    __try
    {
        uintptr_t lods = *reinterpret_cast<const uintptr_t*>(mesh + 0x10);
        uintptr_t lod = *reinterpret_cast<const uintptr_t*>(lods + 0x30);
        uintptr_t sections = *reinterpret_cast<const uintptr_t*>(lod + 8);
        for (int group = 0; group < 4; ++group)
        {
            const uint32_t* entry = reinterpret_cast<const uint32_t*>(lod + 0x10 + group * 12);
            const uint32_t count = entry[0];
            const unsigned char* indices = reinterpret_cast<const unsigned char*>(static_cast<uintptr_t>(entry[1]));
            for (uint32_t i = 0; i < count && i < 64; ++i)
            {
                uintptr_t section = sections + indices[i] * 0x94;
                const uint32_t boneCount = *reinterpret_cast<const unsigned char*>(section + 0x27);
                const uint16_t* list = *reinterpret_cast<const uint16_t* const*>(section + 0x28);
                for (uint32_t b = 0; b < boneCount; ++b)
                    for (int w = 0; w < wantedCount; ++w)
                        if (list[b] == wanted[w])
                            return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

// A batch is drawn by several passes, each through the camera hook, so the arms are solved once and later passes keep
// them: a bone array whose right hand still holds what was written into it is done. ONE RECORD PER ARRAY (2026-11-09,
// "the body did flicker constantly" in two-view mode): it used to be one global record, which one eye per frame never
// broke. With two views each eye has its own array, drawn in several passes on different threads, so the other eye's
// solve replaced the record. The controller had moved a little in between, so this eye's later passes no longer
// matched and solved again on bones already on the controllers: the arms moved twice, a pass at a time. Guarded by
// g_skeletonMutex, as ApplyArmIk.
struct IkWritten
{
    const float* bones = nullptr;
    float hand[12] = {};
};
constexpr int kIkWrittenSlots = 8;
IkWritten g_ikWritten[kIkWrittenSlots];
int g_ikWrittenNext = 0;

bool IkAlreadyWritten(const float* bones, const float* hand)
{
    for (const IkWritten& w : g_ikWritten)
        if (w.bones == bones && memcmp(hand, w.hand, sizeof(w.hand)) == 0)
            return true;
    return false;
}

void NoteIkWritten(const float* bones, const float* hand)
{
    IkWritten* slot = nullptr;
    for (IkWritten& w : g_ikWritten)
        if (w.bones == bones)
            slot = &w;
    if (!slot)
    {
        slot = &g_ikWritten[g_ikWrittenNext];
        g_ikWrittenNext = (g_ikWrittenNext + 1) % kIkWrittenSlots;
    }
    slot->bones = bones;
    memcpy(slot->hand, hand, sizeof(slot->hand));
}

// Settling which skeleton is the first-person one, from a body batch's bones.
constexpr float kBoneMatchMetres = 0.02f; // mean translation difference per bone: 0 = the very copy, a little = a tick old
constexpr DWORD kMatchEveryMs = 100;
constexpr DWORD kBodyGapMs = 2000;        // no body batch for this long (a level load): settle it again
float g_matchBones[kMaxBones * 12];       // guarded by g_skeletonMutex
DWORD g_lastBodyBatchTick = 0, g_lastMatchTick = 0;
uintptr_t g_rejectedSkeleton = 0;

// Mean translation difference of the candidate's bones from these (metres), negative if it can't be read.
float BoneDifference(const SkeletonCandidate& c, const float* batchBones, int count)
{
    if (!ReadBytes(c.bones, g_matchBones, static_cast<size_t>(count) * 48))
        return -1.0f;
    float sum = 0.0f;
    for (int b = 0; b < count * 12; b += 12)
        for (int k = 3; k < 12; k += 4)
            sum += std::fabs(g_matchBones[b + k] - batchBones[b + k]);
    return sum / static_cast<float>(count * 3);
}

} // namespace

// THE RIG AGAINST THE CAMERA (2026-11-01): the bones are posed at the game's tick, and a frame can pair them with a
// camera from a different tick, so on a head turn the whole rig steps against the camera. That shows as a wobble in the
// bone's rotation relative to the batch camera. O = bone * M^-1 is constant for a rig that's rigid to M; its slow
// average O' takes out the bone's own fixed turn, and M' = O'^-1 * bone is the camera the rig really goes with. Only a
// small wobble is that: anything bigger is an animation moving the bone, and following one turned the whole rig and let
// it ease back over the average's second (live: "jump and lerp back", with CameraJoint as the bone).
void AlignRigToBones(Rigid* rigCamera, const float batchRows[3][3][3])
{
    constexpr float kFollowSeconds = 1.0f;
    constexpr float kMaxWobbleDegrees = 3.0f;
    float rows[3][3][3];
    if (batchRows)
    {
        memcpy(rows, batchRows, sizeof(rows));
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        if (!g_haveFrameRows)
            return;
        memcpy(rows, g_frameRows, sizeof(rows));
    }
    static std::mutex mutex;
    static float average[3][3][3];
    static bool haveAverage[3] = {};
    static float peak[3] = {};
    static unsigned long long lastMs = 0, nextLogMs = 0;
    std::lock_guard<std::mutex> lock(mutex);
    const unsigned long long now = GetTickCount64();
    const float dt = lastMs ? static_cast<float>(now - lastMs) * 0.001f : 0.0f;
    lastMs = now;
    const float follow = 1.0f - std::exp(-dt / kFollowSeconds);
    float wobble0 = 1000.0f;
    for (int b = 0; b < 3; ++b)
    {
        if (g_frameBoneIndex[b].load(std::memory_order_relaxed) < 0)
            continue;
        float o[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                o[i][j] = rows[b][i][0] * rigCamera->R[j][0] + rows[b][i][1] * rigCamera->R[j][1] +
                          rows[b][i][2] * rigCamera->R[j][2];
        if (!haveAverage[b])
        {
            memcpy(average[b], o, sizeof(o));
            haveAverage[b] = true;
        }
        float blended[3][3];
        BlendRotation(average[b], o, follow, blended);
        memcpy(average[b], blended, sizeof(blended));
        float trace = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                trace += o[i][j] * average[b][i][j];
        const float cosine = (trace - 1.0f) * 0.5f;
        const float wobble = std::acos(cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine) / kRad;
        if (wobble > peak[b])
            peak[b] = wobble;
        if (b == 0)
            wobble0 = wobble;
    }
    if (now >= nextLogMs)
    {
        nextLogMs = now + 2000;
        MOHW_LOG(kLogFile, "rig wobble against the camera, peak in 2 s: %s %.2f, %s %.2f, %s %.2f deg",
                 kRigFrameBoneNames[0], peak[0], kRigFrameBoneNames[1], peak[1], kRigFrameBoneNames[2], peak[2]);
        peak[0] = peak[1] = peak[2] = 0.0f;
    }
    // Not in a scripted scene (2026-11-12): the rig is the game's then, and easing its turn against the camera made an
    // animated body lag and drift.
    if (!GetStabilizeRig() || wobble0 > kMaxWobbleDegrees || GetPlayerAimDriveWeight() < 1.0f)
        return;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rigCamera->R[i][j] = average[0][0][i] * rows[0][0][j] + average[0][1][i] * rows[0][1][j] +
                                 average[0][2][i] * rows[0][2][j];
}

namespace {

// NATIVE TWO-VIEW BODY (see SyncTwoViewBodyPose in the header). Guarded by g_twoViewPoseMutex.
std::mutex g_twoViewPoseMutex;
unsigned g_twoViewPoseFrame = 0;             // the frame whose first body batch g_twoViewPose holds
const float* g_twoViewPoseSource = nullptr;  // that batch's bone array
const float* g_twoViewPoseTarget = nullptr;  // the other eye's, once given the pose (a batch is drawn by several passes)
int g_twoViewPoseCount = 0;
float g_twoViewPose[kMaxBones * 12];
long g_twoViewPairs = 0, g_twoViewDiffered = 0;
float g_twoViewLargest = 0.0f;
unsigned long long g_twoViewNextLogMs = 0;

bool ReadBatchBones(const unsigned char* batch, float** bones, int* count)
{
    __try
    {
        *bones = *reinterpret_cast<float* const*>(batch + kBatchBones);
        *count = *reinterpret_cast<const int*>(batch + kBatchBoneCount);
        return *bones != nullptr && *count > 0 && *count <= kMaxBones;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CopyFloats(float* to, const float* from, int n)
{
    __try
    {
        memcpy(to, from, static_cast<size_t>(n) * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Largest translation difference over the bones, -1 if unreadable.
float LargestBoneDifference(const float* a, const float* b, int count)
{
    __try
    {
        float largest = 0.0f;
        for (int i = 0; i < count * 12; i += 12)
            for (int k = 3; k < 12; k += 4)
            {
                const float d = std::fabs(a[i + k] - b[i + k]);
                if (d > largest)
                    largest = d;
            }
        return largest;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1.0f;
    }
}

} // namespace

void SyncTwoViewBodyPose(const unsigned char* batch, unsigned frame)
{
    if (frame == 0)
        return;
    float* bones = nullptr;
    int count = 0;
    if (!ReadBatchBones(batch, &bones, &count))
        return;
    std::lock_guard<std::mutex> lock(g_twoViewPoseMutex);
    if (frame != g_twoViewPoseFrame)
    {
        // The first eye's body batch of this frame, before anything moves its bones: its pose is the frame's.
        if (!CopyFloats(g_twoViewPose, bones, count * 12))
            return;
        g_twoViewPoseFrame = frame;
        g_twoViewPoseSource = bones;
        g_twoViewPoseTarget = nullptr;
        g_twoViewPoseCount = count;
        return;
    }
    // One array for both eyes, the one already given the pose (its arms may be on the controllers by now), or another
    // rig: nothing to do.
    if (bones == g_twoViewPoseSource || bones == g_twoViewPoseTarget || count != g_twoViewPoseCount)
        return;
    g_twoViewPoseTarget = bones;
    const float largest = LargestBoneDifference(bones, g_twoViewPose, count);
    ++g_twoViewPairs;
    if (largest != 0.0f && CopyFloats(bones, g_twoViewPose, count * 12))
    {
        ++g_twoViewDiffered;
        if (largest > g_twoViewLargest)
            g_twoViewLargest = largest;
    }
    const unsigned long long now = GetTickCount64();
    if (now >= g_twoViewNextLogMs)
    {
        g_twoViewNextLogMs = now + 5000;
        MOHW_LOG(kLogFile,
                 "native two-view body, last 5 s: %ld frame(s) with a batch per eye, %ld of them with the eyes' poses "
                 "different (largest bone gap %.3f m: copied from different ticks), the second eye given the first's",
                 g_twoViewPairs, g_twoViewDiffered, g_twoViewLargest);
        g_twoViewPairs = 0;
        g_twoViewDiffered = 0;
        g_twoViewLargest = 0.0f;
    }
}

bool ReadBatchFrameRows(const unsigned char* batch, float rows[3][3][3])
{
    float* bones = nullptr;
    int count = 0;
    if (!ReadBatchBones(batch, &bones, &count) || g_frameBoneIndex[0].load(std::memory_order_relaxed) < 0)
        return false;
    memset(rows, 0, sizeof(float) * 27);
    for (int b = 0; b < 3; ++b)
    {
        // As NoteFrameBones: a bone the skeleton lacks stays zero (AlignRigToBones skips it), a bone is the transpose of
        // the row-vector matrix.
        const int index = g_frameBoneIndex[b].load(std::memory_order_relaxed);
        float slot[12];
        if (index < 0 || index >= count || !CopyFloats(slot, bones + index * 12, 12))
        {
            if (b == 0)
                return false;
            continue;
        }
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                rows[b][i][j] = slot[j * 4 + i];
    }
    return true;
}

bool ReadGunSocketInCamera(const unsigned char* batch, const Rigid& rigCamera, float out[3], float handCentre[3])
{
    float* bones = nullptr;
    int count = 0;
    if (!ReadBatchBones(batch, &bones, &count))
        return false;
    float slot[12];
    float bind[3];
    float centreWorld[3];
    {
        std::lock_guard<std::mutex> lock(g_skeletonMutex);
        const Skeleton& sk = g_skeleton;
        const int bone = sk.ikHand[1];
        if (bone < 0 || bone >= sk.boneCount || bone >= static_cast<int>(sk.bind.size()) || count != sk.boneCount ||
            !sk.armsValid || sk.arms[1].hand < 0)
            return false;
        // Only the game's own pose (2026-11-09, "the weapon is now affected by the head, while the hand IK is not"): a
        // batch is drawn by several passes, and after the first one the arm IK has put this array's hand on its
        // controller. Read then, the "hand centre" was the controller's place in the camera, which moves as the head
        // turns, so the socket's offset from it -- and the gun's grip with it -- did too. A pass after the solve keeps
        // the values the first one read.
        float hand[12];
        if (!CopyFloats(hand, BoneSlot(bones, sk.arms[1].hand), 12) || IkAlreadyWritten(bones, hand))
            return false;
        memcpy(bind, sk.bind[bone].data(), sizeof(bind));
        if (!CopyFloats(slot, bones + bone * 12, 12) || !HandCentre(bones, sk, sk.arms[1], centreWorld))
            return false;
    }
    float s[16], world[3];
    ReadBone(slot, s);
    PointThrough(bind, s, world);
    const Rigid inverse = Inverse(rigCamera);
    PointTimes(world, inverse, out);
    PointTimes(centreWorld, inverse, handCentre);
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(out[k]) || !std::isfinite(handCentre[k]) || std::fabs(out[k]) >= 3.0f ||
            std::fabs(handCentre[k]) >= 3.0f)
            return false;
    return true;
}

bool ReadWeaponSocketBones(const unsigned char* batch, const Rigid& rigCamera, bool gunInLeftHand, WeaponSocketBones* out)
{
    out->count = 0;
    float* bones = nullptr;
    int count = 0;
    if (!ReadBatchBones(batch, &bones, &count))
        return false;
    const ArmIkSettings settings = GetArmIkSettings();
    const Rigid rigInverse = Inverse(rigCamera);
    std::lock_guard<std::mutex> lock(g_skeletonMutex);
    const Skeleton& sk = g_skeleton;
    if (!sk.armsValid || count != sk.boneCount || static_cast<int>(sk.parents.size()) != count ||
        static_cast<int>(sk.names.size()) != count || static_cast<int>(sk.bind.size()) != count)
        return false;
    // The game's own pose only, as ReadGunSocketInCamera.
    float hand[12];
    if (!CopyFloats(hand, BoneSlot(bones, sk.arms[1].hand), 12) || IkAlreadyWritten(bones, hand))
        return false;
    static float pose[kMaxBones * 12]; // under g_skeletonMutex
    if (!CopyFloats(pose, bones, count * 12))
        return false;

    // Each hand's grip on its controller, as ApplyArmIk takes it from this pose. With the gun in the left hand the hands
    // trade poses first (MirrorHands), so the hand bones are left out then.
    Rigid gripInverse[2];
    if (!gunInLeftHand)
        for (int h = 0; h < 2; ++h)
        {
            float centre[3], inCamera[3];
            if (!HandCentre(pose, sk, sk.arms[h], centre))
                return false;
            PointTimes(centre, rigInverse, inCamera);
            gripInverse[h] = Inverse(h ? WeaponHandGrip(settings, inCamera, false) : LeftHandGrip(inCamera, false));
        }

    int listed[kMaxBones];
    for (int i = 0; i < count; ++i)
        listed[i] = -1;
    auto add = [&](int bone, int onHand) {
        if (bone < 0 || bone >= count || listed[bone] >= 0 || out->count >= kMaxSocketBones)
            return;
        const int k = out->count++;
        listed[bone] = k;
        snprintf(out->names[k], sizeof(out->names[k]), "%s", sk.names[bone].c_str());
        out->hand[k] = onHand;
        float s[16], world[3];
        ReadBone(BoneSlot(pose, bone), s);
        PointThrough(sk.bind[bone].data(), s, world);
        PointTimes(world, rigInverse, out->joint[k]);
        if (onHand)
            PointTimes(out->joint[k], gripInverse[onHand == 1 ? 1 : 0], out->handGrip[k]);
    };
    for (int i = 0; i < count; ++i)
        if (StartsWith(sk.names[i].c_str(), "Wep_") || StartsWith(sk.names[i].c_str(), "IK_Joint_"))
            add(i, 0);
    if (!gunInLeftHand)
        for (int h = 1; h >= 0; --h)
        {
            const char* weaponBone = h ? "RightWeapon" : "LeftWeapon";
            for (int i = 0; i < count; ++i)
                if (_stricmp(sk.names[i].c_str(), weaponBone) == 0)
                {
                    add(sk.parents[i], h ? 1 : 2); // its hand
                    add(i, h ? 1 : 2);
                }
        }
    for (int i = 0; i < count; ++i)
        if (listed[i] >= 0)
        {
            const int p = sk.parents[i];
            out->parent[listed[i]] = p >= 0 ? listed[p] : -1;
        }
    return out->count > 0;
}

void SetInstantReload(uintptr_t firing, uintptr_t otherFiringsData)
{
    g_instantReloadFiring = firing;
    g_instantReloadData = otherFiringsData;
    if (!otherFiringsData)
        g_otherReloadFiring = 0;
}

uintptr_t GetOtherInstantReloadFiring()
{
    return g_otherReloadFiring;
}

void NoteBodyBatch(const unsigned char* batch)
{
    std::lock_guard<std::mutex> lock(g_skeletonMutex);
    const DWORD now = GetTickCount();
    if (g_skeletonObject.load(std::memory_order_relaxed) && now - g_lastBodyBatchTick > kBodyGapMs)
    {
        MOHW_LOG(kLogFile, "no body batch for a while: finding the skeleton again");
        g_skeleton = Skeleton{};
        g_skeletonObject.store(0, std::memory_order_relaxed);
        g_playerBones.store(0, std::memory_order_relaxed);
        g_magBone.store(-1, std::memory_order_relaxed);
        g_magAmmoBone.store(-1, std::memory_order_relaxed);
        g_rejectedSkeleton = 0;
        g_wantCandidates.store(true, std::memory_order_relaxed);
    }
    g_lastBodyBatchTick = now;
    if (g_skeletonObject.load(std::memory_order_relaxed) || now - g_lastMatchTick < kMatchEveryMs)
        return;
    g_lastMatchTick = now;
    const float* bones = *reinterpret_cast<const float* const*>(batch + kBatchBones);
    const int count = *reinterpret_cast<const int*>(batch + kBatchBoneCount);
    if (!bones || count <= 0 || count > kMaxBones)
        return;

    SkeletonCandidate snapshot[kMaxCandidates];
    int candidates = 0;
    {
        std::lock_guard<std::mutex> candidateLock(g_candidateMutex);
        candidates = g_candidateCount;
        memcpy(snapshot, g_candidates, sizeof(SkeletonCandidate) * candidates);
    }
    float best = -1.0f;
    uintptr_t bestSkeleton = 0, bestBones = 0;
    for (int i = 0; i < candidates; ++i)
    {
        uint32_t namesTable = 0, boneCount = 0;
        if (snapshot[i].skeleton == g_rejectedSkeleton || !ReadU32(snapshot[i].skeleton + kSkeletonNames, &namesTable) ||
            !ReadU32(namesTable + 4, &boneCount) || static_cast<int>(boneCount) != count)
            continue;
        const float difference = BoneDifference(snapshot[i], bones, count);
        if (difference >= 0.0f && (best < 0.0f || difference < best))
        {
            best = difference;
            bestSkeleton = snapshot[i].skeleton;
            bestBones = snapshot[i].bones;
        }
    }
    static std::atomic<int> misses{0};
    if (bestSkeleton == 0 || best > kBoneMatchMetres)
    {
        if (misses.fetch_add(1) < 6)
            MOHW_LOG(kLogFile, "body batch of %d bones: no skeleton matches yet (%d candidates, best %.3f m)", count,
                     candidates, best);
        return;
    }
    MOHW_LOG(kLogFile, "body batch of %d bones is skeleton %p (bones %.4f m apart, of %d candidates)", count,
             reinterpret_cast<void*>(bestSkeleton), best, candidates);
    if (ReadSkeleton(bestSkeleton))
    {
        for (int b = 0; b < 3; ++b)
            g_frameBoneIndex[b].store(g_skeleton.frameBones[b], std::memory_order_relaxed);
        g_playerBones.store(bestBones, std::memory_order_relaxed);
        g_skeletonObject.store(bestSkeleton, std::memory_order_relaxed);
        g_wantCandidates.store(false, std::memory_order_relaxed);
    }
    else
    {
        g_rejectedSkeleton = bestSkeleton;
    }
}

// WEAPON SOCKETS (2026-11-06): the first-person skeleton carries the points the game's own animation holds the hands on
// each gun with, IK_Joint_RightHand (the pistol grip) and IK_Joint_LeftHand (the secondary grip), beside Wep_Root (the
// gun's root, about mid-length and a foot below it), Wep_Aim and Wep_Mag. Once per weapon, 1.5 s after it's equipped
// (the draw animation over), they're measured in the game's own pose, before the arm IK moves anything, and logged to
// mohwvr_weaponsockets.log: relative to Wep_Root, and in the camera's frame (right, up, back -- the frame of the
// weapon grip profile, printed alongside). Under g_skeletonMutex.
void LogWeaponSockets(float* bones, const Skeleton& sk, const Rigid& rigCamera)
{
    constexpr const char* kSocketLog = "mohwvr_weaponsockets.log";
    static unsigned seenGeneration = ~0u, loggedGeneration = ~0u;
    static DWORD changedAt = 0;
    const unsigned generation = GetEquippedWeaponGeneration();
    if (generation == loggedGeneration)
        return;
    const DWORD now = GetTickCount();
    if (generation != seenGeneration)
    {
        seenGeneration = generation;
        changedAt = now;
        return;
    }
    if (now - changedAt < 1500)
        return;
    loggedGeneration = generation;
    char key[160] = "", display[64] = "";
    if (!GetEquippedWeapon(key, sizeof(key), display, sizeof(display)))
        return;
    const Rigid rigInverse = Inverse(rigCamera);
    auto inCamera = [&](int bone, float out[3]) {
        if (bone < 0 || bone >= sk.boneCount || bone >= static_cast<int>(sk.bind.size()))
            return false;
        float s[16], p[3];
        ReadBone(BoneSlot(bones, bone), s);
        PointThrough(sk.bind[bone].data(), s, p);
        PointTimes(p, rigInverse, out);
        return true;
    };
    float root[3];
    if (!inCamera(sk.gunBone, root))
        return;
    std::string line;
    char entry[160];
    snprintf(entry, sizeof(entry), "Wep_Root at (%.3f, %.3f, %.3f) in the camera", root[0], root[1], root[2]);
    line += entry;
    const struct
    {
        const char* name;
        int bone;
    } points[] = {{"IK_Joint_RightHand", sk.ikHand[1]}, {"IK_Joint_LeftHand", sk.ikHand[0]}, {"Wep_Aim", sk.aimBone},
                  {"Wep_Mag", sk.magBone}};
    for (const auto& pt : points)
    {
        float q[3];
        if (!inCamera(pt.bone, q))
            continue;
        snprintf(entry, sizeof(entry), "; %s %+.3f %+.3f %+.3f from Wep_Root (camera %.3f, %.3f, %.3f)", pt.name,
                 q[0] - root[0], q[1] - root[1], q[2] - root[2], q[0], q[1], q[2]);
        line += entry;
    }
    for (int h = 1; h >= 0; --h)
    {
        float centre[3], q[3];
        if (!HandCentre(bones, sk, sk.arms[h], centre))
            continue;
        PointTimes(centre, rigInverse, q);
        snprintf(entry, sizeof(entry), "; %s hand centre (camera %.3f, %.3f, %.3f)", h ? "right" : "left", q[0], q[1], q[2]);
        line += entry;
    }
    int magazineParts = 0;
    for (const auto& slot : g_magazineMeshes)
        magazineParts += slot.load(std::memory_order_relaxed) != 0 ? 1 : 0;
    MOHW_LOG(kSocketLog, "%s (%s): %s; magazine parts %d", display, key, line.c_str(), magazineParts);

    // THE GUN AT ITS HAND SOCKET (2026-11-08): the right-hand socket against the gun's own frame, which the animation
    // doesn't change (the logged camera-frame offsets above turn with the gun). The bones are skinning matrices (model
    // to world, row vectors): a world direction in the gun's model axes is its dot with each of Wep_Root's rows. And
    // each bone's turn in the camera (its skinning rows in the camera's axes: relative to its bind pose, so the same bone
    // compares between guns), for the right hand and the socket. (Until 2026-11-09 this also compared the socket with
    // the weapon profile's grip position, which is gone now.)
    const int socket = sk.ikHand[1], hand = sk.arms[1].hand;
    if (sk.gunBone >= 0 && socket >= 0 && socket < sk.boneCount && socket < static_cast<int>(sk.bind.size()))
    {
        float rootSkin[16], socketSkin[16], rootWorld[3], socketWorld[3];
        ReadBone(BoneSlot(bones, sk.gunBone), rootSkin);
        ReadBone(BoneSlot(bones, socket), socketSkin);
        PointThrough(sk.bind[sk.gunBone].data(), rootSkin, rootWorld);
        PointThrough(sk.bind[socket].data(), socketSkin, socketWorld);
        float d[3], inGun[3];
        for (int k = 0; k < 3; ++k)
            d[k] = socketWorld[k] - rootWorld[k];
        for (int j = 0; j < 3; ++j)
            inGun[j] = d[0] * rootSkin[j * 4 + 0] + d[1] * rootSkin[j * 4 + 1] + d[2] * rootSkin[j * 4 + 2];
        auto rowsInCamera = [&](const float s[16], char* out, size_t size) {
            float r[3][3];
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r[j][k] = s[j * 4 + 0] * rigCamera.R[k][0] + s[j * 4 + 1] * rigCamera.R[k][1] +
                              s[j * 4 + 2] * rigCamera.R[k][2];
            snprintf(out, size, "(%.3f %.3f %.3f)(%.3f %.3f %.3f)(%.3f %.3f %.3f)", r[0][0], r[0][1], r[0][2], r[1][0],
                     r[1][1], r[1][2], r[2][0], r[2][1], r[2][2]);
        };
        char socketRows[128] = "", handRows[128] = "";
        rowsInCamera(socketSkin, socketRows, sizeof(socketRows));
        if (hand >= 0 && hand < sk.boneCount)
        {
            float handSkin[16];
            ReadBone(BoneSlot(bones, hand), handSkin);
            rowsInCamera(handSkin, handRows, sizeof(handRows));
        }
        float socketCam[3];
        PointTimes(socketWorld, rigInverse, socketCam);
        MOHW_LOG(kSocketLog,
                 "  %s IK_Joint_RightHand from Wep_Root in the gun's own frame %+.4f %+.4f %+.4f (%.3f m); in the camera "
                 "(%.3f, %.3f, %.3f); turn in the camera: right hand %s, socket %s",
                 display, inGun[0], inGun[1], inGun[2], std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), socketCam[0],
                 socketCam[1], socketCam[2], handRows[0] ? handRows : "(no hand bone)", socketRows);
    }

    // EVERY WEAPON BONE (2026-11-09, with the socket view, ShowWeaponSockets): each one from its parent (the skeleton's
    // hierarchy), in the gun's own frame (Wep_Root's model axes), and the line's length. Bones on their parent's joint
    // (the parts this gun doesn't have) are counted, not listed.
    if (sk.gunBone >= 0 && static_cast<int>(sk.parents.size()) == sk.boneCount &&
        static_cast<int>(sk.names.size()) == sk.boneCount)
    {
        float rootSkin[16];
        ReadBone(BoneSlot(bones, sk.gunBone), rootSkin);
        auto worldOf = [&](int bone, float p[3]) {
            float s[16];
            ReadBone(BoneSlot(bones, bone), s);
            PointThrough(sk.bind[bone].data(), s, p);
        };
        std::string list;
        int onParent = 0;
        for (int i = 0; i < sk.boneCount; ++i)
        {
            const char* n = sk.names[i].c_str();
            const int p = sk.parents[i];
            if (p < 0 || i == sk.gunBone || !(StartsWith(n, "Wep_") || StartsWith(n, "IK_Joint_")))
                continue;
            float a[3], b[3], d[3], inGun[3];
            worldOf(i, a);
            worldOf(p, b);
            for (int k = 0; k < 3; ++k)
                d[k] = a[k] - b[k];
            const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (length < 0.003f)
            {
                ++onParent;
                continue;
            }
            for (int j = 0; j < 3; ++j)
                inGun[j] = d[0] * rootSkin[j * 4 + 0] + d[1] * rootSkin[j * 4 + 1] + d[2] * rootSkin[j * 4 + 2];
            char entry[160];
            snprintf(entry, sizeof(entry), "%s%s from %s %+.3f %+.3f %+.3f (%.3f m)", list.empty() ? "" : "; ", n,
                     sk.names[p].c_str(), inGun[0], inGun[1], inGun[2], length);
            list += entry;
        }
        MOHW_LOG(kSocketLog, "  %s every weapon bone from its parent, in the gun's frame (hierarchy %s): %s; %d more on "
                 "their parent's joint", display, sk.parentsFromGame ? "from the game" : "by name",
                 list.empty() ? "(none)" : list.c_str(), onParent);
    }
}

bool ApplyArmIk(const unsigned char* batch, const Rigid& cam, const Rigid& rigCamera, const Rigid& drawn, const Rigid& gun,
                const Rigid& weaponHand, const Rigid& offHand, bool haveOffHand, bool twoHanded, bool gunInLeftHand)
{
    std::lock_guard<std::mutex> lock(g_skeletonMutex);
    const Skeleton& sk = g_skeleton;
    if (!sk.armsValid)
        return false;
    float* bones = *reinterpret_cast<float* const*>(batch + kBatchBones);
    int count = *reinterpret_cast<const int*>(batch + kBatchBoneCount);
    if (!bones || count != sk.boneCount)
    {
        static std::atomic<int> logs{0};
        if (logs.fetch_add(1) < 4)
            MOHW_LOG(kLogFile, "body batch: %d bones at %p, the skeleton has %d", count, bones, sk.boneCount);
        return false;
    }
    g_armsDrawnTick.store(GetTickCount(), std::memory_order_relaxed);
    // Several passes and views draw a batch, each through this hook (and from several threads): solved once per array.
    if (IkAlreadyWritten(bones, BoneSlot(bones, sk.arms[1].hand)))
        return true;
    LogWeaponSockets(bones, sk, rigCamera); // the game's own pose, before anything below moves it

    // A point P of this batch shows at P * drawn^-1 * cam; to show it at P * G it has to hold P * G * cam^-1 * drawn.
    const Rigid rigInverse = Inverse(rigCamera);
    const Rigid toRig = Compose(Inverse(cam), drawn);
    const ArmIkSettings settings = GetArmIkSettings();
    if (gunInLeftHand)
    {
        if (sk.mirroredHands.empty())
            return false;
        MirrorHands(bones, sk, rigCamera, rigInverse);
    }
    const int weaponArm = gunInLeftHand ? 0 : 1, offArm = 1 - weaponArm;
    float weaponMove[16], offMove[16];
    {
        // The weapon hand on its own controller (2026-11-09), as the off hand below: it no longer rides the gun.
        float centre[3], inCamera[3];
        if (!HandCentre(bones, sk, sk.arms[weaponArm], centre))
            return false;
        PointTimes(centre, rigInverse, inCamera);
        const Rigid held =
            Compose(Compose(rigInverse, Inverse(WeaponHandGrip(settings, inCamera, gunInLeftHand))), weaponHand);
        ToMatrix4(Compose(held, toRig), weaponMove);
    }
    if (twoHanded || !haveOffHand)
    {
        ToMatrix4(Compose(gun, toRig), offMove); // the gun's own move: keeps the game's own grip on the gun
    }
    else
    {
        float centre[3], inCamera[3];
        if (!HandCentre(bones, sk, sk.arms[offArm], centre))
            return false;
        PointTimes(centre, rigInverse, inCamera);
        const Rigid held = Compose(Compose(rigInverse, Inverse(LeftHandGrip(inCamera, gunInLeftHand))), offHand);
        ToMatrix4(Compose(held, toRig), offMove);
    }

    // The body's frame in the rig: level, facing where the camera does.
    float forward[3] = {-cam.R[2][0], 0.0f, -cam.R[2][2]}, right[3] = {cam.R[0][0], 0.0f, cam.R[0][2]};
    if (!Normalize3(forward) || !Normalize3(right))
        return false;
    // The controllers' forward and down (the frames' rows: right, up, back), rig space.
    float rot[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rot[i * 3 + j] = toRig.R[i][j];
    float fwd[2][3], dwn[2][3];
    const Rigid& offFrame = haveOffHand ? offHand : gun;
    const float offF[3] = {-offFrame.R[2][0], -offFrame.R[2][1], -offFrame.R[2][2]};
    const float offD[3] = {-offFrame.R[1][0], -offFrame.R[1][1], -offFrame.R[1][2]};
    VecTimes(offF, rot, fwd[offArm]);
    VecTimes(offD, rot, dwn[offArm]);
    // The weapon hand's controller, as the off hand's.
    const float weaponF[3] = {-weaponHand.R[2][0], -weaponHand.R[2][1], -weaponHand.R[2][2]};
    const float weaponD[3] = {-weaponHand.R[1][0], -weaponHand.R[1][1], -weaponHand.R[1][2]};
    VecTimes(weaponF, rot, fwd[weaponArm]);
    VecTimes(weaponD, rot, dwn[weaponArm]);

    // A mirrored hand's turn isn't its move, so the forearm twist can't be read from it: the forearm doesn't twist then.
    const bool twist = !gunInLeftHand;
    // The head: the camera the rig was posed against (cam's own position is the eye in two-view mode, and the arms are
    // solved once for both eyes).
    SolveArm(settings, bones, sk, weaponArm, weaponMove, forward, right, rigCamera.t, fwd[weaponArm], dwn[weaponArm],
             twist);
    SolveArm(settings, bones, sk, offArm, offMove, forward, right, rigCamera.t, fwd[offArm], dwn[offArm], twist);
    NoteIkWritten(bones, BoneSlot(bones, sk.arms[1].hand));
    return true;
}

bool ShouldDrawArmMesh(uintptr_t mesh)
{
    if (GetTickCount() - g_armsDrawnTick.load(std::memory_order_relaxed) > 300)
        return false;
    {
        std::lock_guard<std::mutex> lock(g_meshClassMutex);
        for (int i = 0; i < g_meshClassCount; ++i)
            if (g_meshClasses[i].mesh == mesh)
                return g_meshClasses[i].arm;
    }
    int wanted[4], magazine[2];
    {
        std::lock_guard<std::mutex> lock(g_skeletonMutex);
        if (!g_skeleton.armsValid)
            return false;
        wanted[0] = g_skeleton.arms[0].fore;
        wanted[1] = g_skeleton.arms[1].fore;
        wanted[2] = g_skeleton.arms[0].hand;
        wanted[3] = g_skeleton.arms[1].hand;
        magazine[0] = g_skeleton.magBone;
        magazine[1] = g_skeleton.magAmmoBone;
    }
    const bool arm = MeshUsesBones(mesh, wanted, 4);
    // A gun whose magazine is no separate part (the pistols) may have it skinned to Wep_Mag in its own mesh, a part of
    // this batch: moving that bone in the batch, as the arm IK moves the hands, would move the magazine.
    const bool skinnedMagazine = magazine[0] >= 0 && MeshUsesBones(mesh, magazine, magazine[1] >= 0 ? 2 : 1);
    std::lock_guard<std::mutex> lock(g_meshClassMutex);
    if (g_meshClassCount < kMaxMeshClasses)
        g_meshClasses[g_meshClassCount++] = MeshClass{mesh, arm};
    char weapon[160] = "", display[64] = "";
    GetEquippedWeapon(weapon, sizeof(weapon), display, sizeof(display));
    MOHW_LOG(kLogFile, "body part mesh %p: %s%s (weapon %s)", reinterpret_cast<void*>(mesh), arm ? "arms / hands" : "not the arms",
             skinnedMagazine ? ", SKINNED TO Wep_Mag (the magazine is in this mesh)" : "", display[0] ? display : "?");
    return arm;
}

void NoteWeaponMesh(uintptr_t mesh)
{
    // Once per mesh: the guns with no separate magazine part (the pistols: one first-person mesh each in the data) may
    // carry it skinned to Wep_Mag, which a bone rewrite could move.
    constexpr int kMaxSeen = 64;
    static std::atomic<uintptr_t> seen[kMaxSeen];
    if (!mesh)
        return;
    for (auto& slot : seen)
    {
        uintptr_t held = slot.load(std::memory_order_relaxed);
        if (held == mesh)
            return;
        if (held == 0)
        {
            uintptr_t expected = 0;
            if (!slot.compare_exchange_strong(expected, mesh, std::memory_order_relaxed))
            {
                if (expected == mesh)
                    return;
                continue;
            }
            int magazine[2];
            {
                std::lock_guard<std::mutex> lock(g_skeletonMutex);
                magazine[0] = g_skeleton.magBone;
                magazine[1] = g_skeleton.magAmmoBone;
            }
            if (magazine[0] < 0)
                return;
            const bool skinned = MeshUsesBones(mesh, magazine, magazine[1] >= 0 ? 2 : 1);
            char weapon[160] = "", display[64] = "";
            GetEquippedWeapon(weapon, sizeof(weapon), display, sizeof(display));
            MOHW_LOG(kLogFile, "weapon mesh %p (weapon %s): %s", reinterpret_cast<void*>(mesh), display[0] ? display : "?",
                     skinned ? "SKINNED TO Wep_Mag (the magazine is in this mesh)" : "not skinned to Wep_Mag");
            return;
        }
    }
}

bool IsMagazineMesh(uintptr_t mesh)
{
    unsigned generation = GetEquippedWeaponGeneration();
    if (generation != g_magazineGeneration.load(std::memory_order_relaxed))
    {
        g_magazineGeneration.store(generation, std::memory_order_relaxed);
        for (auto& slot : g_magazineMeshes)
            slot.store(0, std::memory_order_relaxed);
    }
    for (const auto& slot : g_magazineMeshes)
        if (slot.load(std::memory_order_relaxed) == mesh)
            return true;
    return false;
}

bool GetMagazineBonePoint(float out[3])
{
    int bone = g_magBone.load(std::memory_order_relaxed);
    uintptr_t skeleton = g_skeletonObject.load(std::memory_order_relaxed);
    if (bone < 0 || !skeleton)
        return false;
    uint32_t array = 0;
    float b[12];
    if (!ReadU32(skeleton + kSkeletonBones, &array) || !array || !ReadBytes(array + bone * 0x30, b, sizeof(b)))
        return false;
    float bind[3];
    {
        std::lock_guard<std::mutex> lock(g_skeletonMutex);
        if (bone >= static_cast<int>(g_skeleton.bind.size()))
            return false;
        memcpy(bind, g_skeleton.bind[bone].data(), sizeof(bind));
    }
    for (int i = 0; i < 3; ++i)
        out[i] = b[i * 4] * bind[0] + b[i * 4 + 1] * bind[1] + b[i * 4 + 2] * bind[2] + b[i * 4 + 3];
    return true;
}

bool InstallFirstPersonRigHooks()
{
    if (g_moduleBase == 0 || !g_buildVerified)
        return false;
    MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    g_bonesGetterAddress = Offset<void*>(OFFSET_FP_BONES_GETTER);
    MH_STATUS s = MH_CreateHook(g_bonesGetterAddress, reinterpret_cast<void*>(&HookedBonesGetter),
                                reinterpret_cast<void**>(&g_originalBonesGetter));
    if (s == MH_OK)
        s = MH_EnableHook(g_bonesGetterAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "bones getter hook (FUN_009551F0 @ %p) FAILED: %s", g_bonesGetterAddress, MH_StatusToString(s));
        g_bonesGetterAddress = nullptr;
        return false;
    }

    g_placePartAddress = Offset<void*>(OFFSET_WEAPON_PART_PLACE);
    s = MH_CreateHook(g_placePartAddress, reinterpret_cast<void*>(&HookedPlacePart),
                      reinterpret_cast<void**>(&g_originalPlacePart));
    if (s == MH_OK)
        s = MH_EnableHook(g_placePartAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "weapon part hook (FUN_00A37160 @ %p) FAILED: %s -- no manual reload", g_placePartAddress,
                 MH_StatusToString(s));
        g_placePartAddress = nullptr;
    }
    g_reloadTimeCallersFirst = g_baseDelta + OFFSET_RELOAD_TIME_CALLERS_FIRST;
    g_reloadTimeAddress = Offset<void*>(OFFSET_RELOAD_TIME);
    s = MH_CreateHook(g_reloadTimeAddress, reinterpret_cast<void*>(&ReloadTimeStub), &g_originalReloadTime);
    if (s == MH_OK)
        s = MH_EnableHook(g_reloadTimeAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "reload time hook (FUN_00831080 @ %p) FAILED: %s", g_reloadTimeAddress, MH_StatusToString(s));
        g_reloadTimeAddress = nullptr;
    }
    g_reloadStartedAddress = Offset<void*>(OFFSET_RELOAD_STARTED_NOTIFY);
    s = MH_CreateHook(g_reloadStartedAddress, reinterpret_cast<void*>(&HookedReloadStarted),
                      reinterpret_cast<void**>(&g_originalReloadStarted));
    if (s == MH_OK)
        s = MH_EnableHook(g_reloadStartedAddress);
    if (s != MH_OK)
    {
        MOHW_LOG(kLogFile, "reload notify hook (FUN_008373E0 @ %p) FAILED: %s -- the reload animation will play",
                 g_reloadStartedAddress, MH_StatusToString(s));
        g_reloadStartedAddress = nullptr;
    }
    MOHW_LOG(kLogFile, "first-person rig hooks installed");
    return true;
}

void RemoveFirstPersonRigHooks()
{
    for (void** address : {&g_bonesGetterAddress, &g_placePartAddress, &g_reloadStartedAddress, &g_reloadTimeAddress})
    {
        if (*address)
        {
            MH_DisableHook(*address);
            MH_RemoveHook(*address);
            *address = nullptr;
        }
    }
}

} // namespace mohw
