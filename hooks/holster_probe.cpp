#include "holster_probe.h"

#include "weapon_identity.h"
#include "../sdk/logging.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace mohw {
namespace {

constexpr const char* kLogFile = "mohwvr_holsterprobe.log";
constexpr int kMaxWeapons = 12;
constexpr int kMaxBatches = 24;
constexpr int kMaxPairs = 8;
constexpr int kHashBytes = 64;
constexpr unsigned long long kFirstSummaryMs = 3000;
constexpr unsigned long long kSummaryEveryMs = 15000;
// The lifetime checks (summaries in hand, the checks after putting a gun away, the comparisons on taking it out again,
// the raw dumps) answered their question on 2026-11-17: a weapon's meshes and their "second" objects stay loaded and
// unchanged while it's put away (60 s and more), and come back as the same pointers; only the batch descriptor, its
// pair array, records and +0x98 are rebuilt every frame. Off since; the in-hand batch list stays, with batches leaving
// the view and coming back (kOutOfViewMs without a sighting) for the magnifier toggle.
constexpr bool kLifetimeChecks = false;
constexpr unsigned long long kOutOfViewMs = 300;
const int kCheckSeconds[] = {1, 2, 5, 10, 30, 60};
constexpr int kCheckCount = static_cast<int>(sizeof(kCheckSeconds) / sizeof(kCheckSeconds[0]));

bool SafeRead(void* dst, uintptr_t src, size_t n)
{
    if (src < 0x10000 || src > 0xFFFF0000u - n)
        return false;
    __try
    {
        memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

uint32_t Fnv(const unsigned char* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

// What sits at a pointer: its first dword (a vtable, for an object) and a hash of its first kHashBytes. ok = readable.
struct Content
{
    bool ok = false;
    uint32_t type = 0;
    uint32_t hash = 0;
};

Content Look(uintptr_t p)
{
    Content c{};
    unsigned char bytes[kHashBytes];
    if (!SafeRead(bytes, p, sizeof(bytes)))
        return c;
    c.ok = true;
    memcpy(&c.type, bytes, 4);
    c.hash = Fnv(bytes, sizeof(bytes));
    return c;
}

bool ContainsMesh(const char* s)
{
    for (; *s; ++s)
        if ((s[0] | 0x20) == 'm' && (s[1] | 0x20) == 'e' && (s[2] | 0x20) == 's' && (s[3] | 0x20) == 'h')
            return true;
    return false;
}

// The mesh's asset name: a pointer among its first dwords leads to an object holding it at +0x58 (as
// hooks/camera_matrix_test_hook.cpp MeshAssetName reads it).
bool ReadName(uintptr_t mesh, char* out, int size)
{
    out[0] = 0;
    uintptr_t words[32];
    if (!SafeRead(words, mesh, sizeof(words)))
        return false;
    for (uintptr_t w : words)
    {
        uintptr_t s = 0;
        char buf[128];
        if (!SafeRead(&s, w + 0x58, 4) || !SafeRead(buf, s, sizeof(buf) - 1))
            continue;
        buf[sizeof(buf) - 1] = 0;
        int n = 0;
        while (n < 127 && buf[n] >= 0x20 && buf[n] <= 0x7E)
            ++n;
        if (n >= 8 && buf[n] == 0 && ContainsMesh(buf))
        {
            const char* slash = strrchr(buf, '/');
            strncpy_s(out, size, slash ? slash + 1 : buf, _TRUNCATE);
            return true;
        }
    }
    return false;
}

// The batch fields watched (offsets from the batch: hooks/camera_matrix_test_hook.cpp and the batch notes).
struct Fields
{
    uintptr_t batch = 0, p74 = 0, inst = 0, recs = 0, p98 = 0, pA0 = 0;
    uint32_t count = 0;
    uintptr_t mesh[kMaxPairs] = {}, second[kMaxPairs] = {};
};

bool ReadFields(const unsigned char* b, Fields* f)
{
    f->batch = reinterpret_cast<uintptr_t>(b);
    if (!SafeRead(&f->p74, f->batch + 0x74, 4) || !SafeRead(&f->inst, f->batch + 0x78, 4) ||
        !SafeRead(&f->count, f->batch + 0x7C, 4) || !SafeRead(&f->recs, f->batch + 0x80, 4) ||
        !SafeRead(&f->p98, f->batch + 0x98, 4) || !SafeRead(&f->pA0, f->batch + 0xA0, 4))
        return false;
    if (f->count == 0 || f->count > 64)
        return false;
    const uint32_t n = f->count < kMaxPairs ? f->count : kMaxPairs;
    uintptr_t pairs[kMaxPairs * 2] = {};
    if (!SafeRead(pairs, f->inst, n * 8))
        return false;
    for (uint32_t i = 0; i < n; ++i)
    {
        f->mesh[i] = pairs[i * 2];
        f->second[i] = pairs[i * 2 + 1];
    }
    return true;
}

// One batch of one weapon.
struct BatchRec
{
    uint32_t key = 0; // first mesh ^ count << 28
    char name[96] = "";
    Fields last{};
    Content secondData[kMaxPairs], recsData, p98Data, pA0Data; // the latest
    // This window: observations, and how often each field / the data behind it changed from the previous one.
    int seen = 0, batchMoved = 0, p74Changed = 0, instMoved = 0, recsMoved = 0, p98Moved = 0, pA0Moved = 0, meshChanged = 0,
        secondChanged = 0, secondDataChanged = 0, recsDataChanged = 0;
    // At holster time, to compare with.
    Fields atHolster{};
    Content holsterSecond[kMaxPairs], holsterRecs, holsterP98, holsterPA0, holsterMesh[kMaxPairs];
    bool comparedOnReturn = false;
    unsigned long long lastSeenMs = 0;
    bool visible = true;
};

struct WeaponRec
{
    bool used = false;
    char key[128] = "", display[64] = "";
    unsigned generation = 0;
    BatchRec batches[kMaxBatches];
    int batchCount = 0;
    bool dumped = false;
    bool inHand = false;
    unsigned long long equippedMs = 0, nextSummaryMs = 0, holsteredMs = 0;
    int nextCheck = kCheckCount; // the holstered checks done so far
    int returns = 0;
};

std::mutex g_mutex;
WeaponRec g_weapons[kMaxWeapons];
int g_current = -1;
unsigned g_currentGeneration = ~0u;

void DumpWords(const char* what, uintptr_t p, int words)
{
    uint32_t w[64] = {};
    if (words > 64)
        words = 64;
    if (!SafeRead(w, p, static_cast<size_t>(words) * 4))
    {
        MOHW_LOG_ALWAYS(kLogFile, "  dump %s %08X: not readable", what, static_cast<unsigned>(p));
        return;
    }
    for (int row = 0; row < words; row += 8)
    {
        char line[160];
        int used = sprintf_s(line, "  dump %s %08X +%02X:", what, static_cast<unsigned>(p), row * 4);
        for (int i = row; i < row + 8 && i < words; ++i)
            used += sprintf_s(line + used, sizeof(line) - used, " %08X", w[i]);
        MOHW_LOG_ALWAYS(kLogFile, "%s", line);
    }
}

void SnapshotForHolster(WeaponRec& w, unsigned long long now)
{
    w.inHand = false;
    w.holsteredMs = now;
    w.nextCheck = 0;
    for (int i = 0; i < w.batchCount; ++i)
    {
        BatchRec& r = w.batches[i];
        r.atHolster = r.last;
        const uint32_t n = r.last.count < kMaxPairs ? r.last.count : kMaxPairs;
        for (uint32_t k = 0; k < n; ++k)
        {
            r.holsterSecond[k] = Look(r.last.second[k]);
            r.holsterMesh[k] = Look(r.last.mesh[k]);
        }
        r.holsterRecs = Look(r.last.recs);
        r.holsterP98 = Look(r.last.p98);
        r.holsterPA0 = Look(r.last.pA0);
        r.comparedOnReturn = false;
    }
    if (kLifetimeChecks)
        MOHW_LOG_ALWAYS(kLogFile, "PUT AWAY: %s (%d batches) -- checking what it left behind at 1, 2, 5, 10, 30, 60 s",
                        w.display, w.batchCount);
    else
        MOHW_LOG_ALWAYS(kLogFile, "PUT AWAY: %s (%d batches)", w.display, w.batchCount);
}

// Same: still readable with the same type word and bytes; type: same type word, different bytes; gone: unreadable or a
// different type word.
struct Tally
{
    int same = 0, typeOnly = 0, gone = 0;
    void Add(const Content& then, const Content& now)
    {
        if (!then.ok)
            return;
        if (!now.ok || now.type != then.type)
            ++gone;
        else if (now.hash == then.hash)
            ++same;
        else
            ++typeOnly;
    }
};

void CheckHolstered(WeaponRec& w, unsigned long long now)
{
    while (w.nextCheck < kCheckCount && now - w.holsteredMs >= static_cast<unsigned long long>(kCheckSeconds[w.nextCheck]) * 1000)
    {
        Tally meshes, seconds, recs, p98, pA0, batches;
        for (int i = 0; i < w.batchCount; ++i)
        {
            BatchRec& r = w.batches[i];
            const uint32_t n = r.atHolster.count < kMaxPairs ? r.atHolster.count : kMaxPairs;
            for (uint32_t k = 0; k < n; ++k)
            {
                meshes.Add(r.holsterMesh[k], Look(r.atHolster.mesh[k]));
                seconds.Add(r.holsterSecond[k], Look(r.atHolster.second[k]));
            }
            recs.Add(r.holsterRecs, Look(r.atHolster.recs));
            p98.Add(r.holsterP98, Look(r.atHolster.p98));
            pA0.Add(r.holsterPA0, Look(r.atHolster.pA0));
            // The last batch descriptor: does it still describe this weapon (its pair array and count)?
            Fields still{};
            const bool same = ReadFields(reinterpret_cast<const unsigned char*>(r.atHolster.batch), &still) &&
                              still.inst == r.atHolster.inst && still.count == r.atHolster.count &&
                              still.mesh[0] == r.atHolster.mesh[0];
            same ? ++batches.same : ++batches.gone;
        }
        MOHW_LOG_ALWAYS(kLogFile,
                        "put away %s +%d s: meshes %d same / %d changed / %d gone | second's data %d / %d / %d | records %d "
                        "/ %d / %d | +0x98 %d / %d / %d | +0xA0 %d / %d / %d | last descriptors still this weapon %d of %d",
                        w.display, kCheckSeconds[w.nextCheck], meshes.same, meshes.typeOnly, meshes.gone, seconds.same,
                        seconds.typeOnly, seconds.gone, recs.same, recs.typeOnly, recs.gone, p98.same, p98.typeOnly, p98.gone,
                        pA0.same, pA0.typeOnly, pA0.gone, batches.same, batches.same + batches.gone);
        ++w.nextCheck;
    }
}

void Summarize(WeaponRec& w, unsigned long long now)
{
    MOHW_LOG_ALWAYS(kLogFile, "IN HAND %s, %.0f s: %d batches; per batch, changes over the observations since the last summary:",
                    w.display, (now - w.equippedMs) / 1000.0, w.batchCount);
    for (int i = 0; i < w.batchCount; ++i)
    {
        BatchRec& r = w.batches[i];
        MOHW_LOG_ALWAYS(kLogFile,
                        "  #%d %-40s count %u, seen %d: descriptor moved %d, +0x74 %d, pairs array %d, records %d, +0x98 %d, "
                        "+0xA0 %d | meshes changed %d, second ptrs %d | second's data changed %d, records' data %d | now: "
                        "second[0] %08X (type %08X), records %08X, +0x98 %08X, +0xA0 %08X",
                        i, r.name[0] ? r.name : "(no name)", r.last.count, r.seen, r.batchMoved, r.p74Changed, r.instMoved,
                        r.recsMoved, r.p98Moved, r.pA0Moved, r.meshChanged, r.secondChanged, r.secondDataChanged,
                        r.recsDataChanged, static_cast<unsigned>(r.last.second[0]), r.secondData[0].type,
                        static_cast<unsigned>(r.last.recs), static_cast<unsigned>(r.last.p98),
                        static_cast<unsigned>(r.last.pA0));
        r.seen = r.batchMoved = r.p74Changed = r.instMoved = r.recsMoved = r.p98Moved = r.pA0Moved = r.meshChanged =
            r.secondChanged = r.secondDataChanged = r.recsDataChanged = 0;
    }
}

int FindOrAddWeapon(const char* key, const char* display)
{
    for (int i = 0; i < kMaxWeapons; ++i)
        if (g_weapons[i].used && strcmp(g_weapons[i].key, key) == 0)
            return i;
    for (int i = 0; i < kMaxWeapons; ++i)
        if (!g_weapons[i].used)
        {
            g_weapons[i].used = true;
            strncpy_s(g_weapons[i].key, key, _TRUNCATE);
            strncpy_s(g_weapons[i].display, display, _TRUNCATE);
            return i;
        }
    return -1;
}

} // namespace

void HolsterProbeOnBatch(const unsigned char* batch)
{
    Fields f{};
    if (!batch || !ReadFields(batch, &f))
        return;
    const unsigned long long now = GetTickCount64();
    const unsigned generation = GetEquippedWeaponGeneration();
    std::lock_guard<std::mutex> lock(g_mutex);

    if (generation != g_currentGeneration)
    {
        char key[128] = "", display[64] = "";
        if (!GetEquippedWeapon(key, sizeof(key), display, sizeof(display)) || !key[0])
            return; // not known yet: wait for the identity
        if (g_current >= 0 && g_weapons[g_current].inHand)
            SnapshotForHolster(g_weapons[g_current], now);
        g_currentGeneration = generation;
        g_current = FindOrAddWeapon(key, display);
        if (g_current < 0)
            return;
        WeaponRec& w = g_weapons[g_current];
        const bool back = w.batchCount > 0;
        if (back)
        {
            ++w.returns;
            MOHW_LOG_ALWAYS(kLogFile, "TAKEN OUT AGAIN: %s (%.1f s after it was put away); comparing its batches as they come",
                            w.display, (now - w.holsteredMs) / 1000.0);
        }
        else
            MOHW_LOG_ALWAYS(kLogFile, "IN HAND: %s (%s)", w.display, w.key);
        w.generation = generation;
        w.inHand = true;
        w.nextCheck = kCheckCount; // no more holstered checks while it's in hand
        w.equippedMs = now;
        w.nextSummaryMs = now + kFirstSummaryMs;
        for (int i = 0; i < w.batchCount; ++i) // the view check starts afresh (it only runs for the gun in hand)
        {
            w.batches[i].lastSeenMs = now;
            w.batches[i].visible = true;
        }
    }
    if (g_current < 0)
        return;
    WeaponRec& w = g_weapons[g_current];

    // This batch's record.
    const uint32_t key = static_cast<uint32_t>(f.mesh[0]) ^ (f.count << 28);
    BatchRec* r = nullptr;
    for (int i = 0; i < w.batchCount && !r; ++i)
        if (w.batches[i].key == key)
            r = &w.batches[i];
    const uint32_t n = f.count < kMaxPairs ? f.count : kMaxPairs;
    if (!r)
    {
        // Another weapon's batch: the one put away is still drawn while it lowers after the identity has moved on.
        for (int i = 0; i < kMaxWeapons; ++i)
            if (i != g_current && g_weapons[i].used)
                for (int k = 0; k < g_weapons[i].batchCount; ++k)
                    if (g_weapons[i].batches[k].key == key)
                        return;
        if (w.batchCount >= kMaxBatches)
            return;
        r = &w.batches[w.batchCount++];
        r->key = key;
        ReadName(f.mesh[0], r->name, sizeof(r->name));
        r->last = f;
        r->lastSeenMs = now;
        r->visible = true;
        for (uint32_t k = 0; k < n; ++k)
            r->secondData[k] = Look(f.second[k]);
        r->recsData = Look(f.recs);
        MOHW_LOG_ALWAYS(kLogFile, "  +%6.1f s  batch #%d of %s: %s, count %u, mesh[0] %08X second[0] %08X records %08X",
                        (now - w.equippedMs) / 1000.0, w.batchCount - 1, w.display, r->name[0] ? r->name : "(no name)",
                        f.count, static_cast<unsigned>(f.mesh[0]), static_cast<unsigned>(f.second[0]),
                        static_cast<unsigned>(f.recs));
        if (kLifetimeChecks && !w.dumped)
        {
            w.dumped = true;
            MOHW_LOG_ALWAYS(kLogFile, "  raw dumps for %s, its first batch:", w.display);
            DumpWords("batch", f.batch, 0xD0 / 4);
            DumpWords("second[0]", f.second[0], 32);
            DumpWords("mesh[0]", f.mesh[0], 16);
            DumpWords("records", f.recs, 0x50 / 4 * 2);
            DumpWords("+0x98", f.p98, 16);
            DumpWords("+0xA0", f.pA0, 16);
        }
        return;
    }

    if (!r->visible)
    {
        r->visible = true;
        MOHW_LOG_ALWAYS(kLogFile, "  +%6.1f s  back in view: %s", (now - w.equippedMs) / 1000.0,
                        r->name[0] ? r->name : "(no name)");
    }
    r->lastSeenMs = now;

    // Taken out again: the first sight of each batch, against what it was when put away.
    if (kLifetimeChecks && w.returns > 0 && !r->comparedOnReturn && r->atHolster.count)
    {
        r->comparedOnReturn = true;
        int sameMesh = 0, sameSecond = 0;
        for (uint32_t k = 0; k < n; ++k)
        {
            sameMesh += f.mesh[k] == r->atHolster.mesh[k];
            sameSecond += f.second[k] == r->atHolster.second[k];
        }
        MOHW_LOG_ALWAYS(kLogFile, "  back: %s: meshes %d of %u the same, second ptrs %d of %u, records %s, +0x98 %s, +0xA0 %s",
                        r->name[0] ? r->name : "(no name)", sameMesh, n, sameSecond, n,
                        f.recs == r->atHolster.recs ? "same" : "new", f.p98 == r->atHolster.p98 ? "same" : "new",
                        f.pA0 == r->atHolster.pA0 ? "same" : "new");
    }

    ++r->seen;
    r->batchMoved += f.batch != r->last.batch;
    r->p74Changed += f.p74 != r->last.p74;
    r->instMoved += f.inst != r->last.inst;
    r->recsMoved += f.recs != r->last.recs;
    r->p98Moved += f.p98 != r->last.p98;
    r->pA0Moved += f.pA0 != r->last.pA0;
    bool meshChanged = false, secondChanged = false, dataChanged = false;
    for (uint32_t k = 0; k < n; ++k)
    {
        meshChanged |= f.mesh[k] != r->last.mesh[k];
        secondChanged |= f.second[k] != r->last.second[k];
        const Content c = Look(f.second[k]);
        dataChanged |= c.hash != r->secondData[k].hash;
        r->secondData[k] = c;
    }
    r->meshChanged += meshChanged;
    r->secondChanged += secondChanged;
    r->secondDataChanged += dataChanged;
    const Content recs = Look(f.recs);
    r->recsDataChanged += recs.hash != r->recsData.hash;
    r->recsData = recs;
    r->last = f;

    if (kLifetimeChecks && now >= w.nextSummaryMs)
    {
        Summarize(w, now);
        w.nextSummaryMs = now + kSummaryEveryMs;
    }
    if (kLifetimeChecks)
        for (int i = 0; i < kMaxWeapons; ++i)
            if (g_weapons[i].used && !g_weapons[i].inHand && g_weapons[i].nextCheck < kCheckCount)
                CheckHolstered(g_weapons[i], now);

    // Batches of the gun in hand leaving the view and coming back (a flip-up magnifier, the zoom optic's mesh at ADS).
    static unsigned long long nextVisibilityCheck = 0;
    if (now >= nextVisibilityCheck)
    {
        nextVisibilityCheck = now + 100;
        for (int i = 0; i < w.batchCount; ++i)
        {
            BatchRec& b = w.batches[i];
            if (b.visible && now - b.lastSeenMs > kOutOfViewMs)
            {
                b.visible = false;
                MOHW_LOG_ALWAYS(kLogFile, "  +%6.1f s  out of view: %s", (now - w.equippedMs) / 1000.0,
                                b.name[0] ? b.name : "(no name)");
            }
        }
    }
}

} // namespace mohw
