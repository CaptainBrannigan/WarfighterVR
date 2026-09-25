// NOT COMPILED. Reference-only dump of the remaining reversed classes from
// the original SDK paste (player/soldier/weapon/physics/entity layouts).
// None of this is needed for v1 (head-tracked stereo rendering only touches
// DxRenderer/RenderView/GameRenderer, see sdk/dxrenderer.h and
// sdk/renderview.h) -- it's kept here so the offsets aren't lost, for when
// motion-controller aiming / player-relative tracking becomes in scope.
//
// This block is NOT wired into any project and is not guaranteed to compile:
// several referenced types (Client, MemoryArena, TypeInfo, GameTime,
// MaterialGridManager, HookCollisionInfo, HookProximityInfo, WorldRenderModule,
// GameWorld, ant::AnimationSkeleton, EntityBusPeer, ComponentData, ComponentInfo,
// EntryComponent, ClientEntryComponent, network::ClientGhost,
// network::IClientNetworkable, IRigidBodyHook, WeaponSway, WeaponFiring,
// CharacterPhysicsEntity, SupportedShootingCallback, EyePositionCallback,
// SoldierEntityActionState, CharacterPoseType, PersonViewEnum,
// ClientSoldierPrediction, ClientSoldierReplication, ClientSoldierWeaponsComponent,
// ClientSoldierWeapon, MaterialContainerPair, BreathControlData,
// BoneCollisionComponentData, RayCastHit, PhysicsEntityBase, WeakPtr<T>,
// String, Array<T>, eastl::vector/basic_string/pair, WeaponSuppressionData,
// FiringDispersionData, SoldierWeaponDispersion, FireEffectData,
// ShotConfigData deps, FireLogicData, AmmoConfigData, OverHeatData -- are
// used only as opaque pointers/values in the original dump and were never
// forward-declared there either. Before compiling any of this, add forward
// declarations (or real definitions, if reversed) for whatever subset a
// future phase actually touches.
//
// One fix applied vs. the raw paste: the original had a stray extra `};`
// after ClientGameContext's closing brace (a copy-paste artifact -- likely
// the tail of an outer `namespace fb { ... }` whose opening line wasn't
// included when the dump was captured). That stray brace is removed below
// since GameContext/ClientGameContext are already correctly closed without it.

#if 0

// --- content preserved verbatim from the user's original paste, minus the ---
// --- stray brace noted above and the pieces already promoted to real,     ---
// --- compiled headers (Main, PlayerManager, GameContext, ClientGameContext,---
// --- BorderInputNode, InputCache, DxRenderer*, RenderView*, GameRenderer*) ---

namespace ant
{
class UpdatePoseResultDataInternal
{
public:
    fb::QuatTransform * m_localTransforms;
    fb::QuatTransform * m_worldTransforms;
    LPD3DXMATRIX  m_renderTransforms;
    fb::QuatTransform * m_interpolatedLocalTransforms;
    fb::QuatTransform * m_interpolatedWorldTransforms;
    fb::QuatTransform * m_activeWorldTransforms;
    fb::QuatTransform * m_activeLocalTransforms;
    int m_slot;
    int m_readerIndex;
    byte m_validTransforms;
    byte m_poseUpdateEnabled;
    byte m_poseNeeded;
    PAD(0x1);
};

class UpdatePoseResultData : public UpdatePoseResultDataInternal
{
};
};

class OnlineId
{
public:
    ULONGLONG m_nativeData;
    CHAR m_szId[0x11];
    PAD(0x7);
};

class WeakTokenHolder
{
    volatile unsigned int m_token;
};

class IPhysicsRayCaster
{
public:
    virtual bool physicsRayQuery(char* text, Vec3 *from, Vec3 *to, RayCastHit *hit, int flag, void* PhysicsEntityList);
    virtual struct SafeQueryResult * asyncPhysicsRayQuery(const char *ident, Vec3 *from, Vec3 *to, unsigned int flags, void* excluded);

    static IPhysicsRayCaster* Singleton(void) { return *(IPhysicsRayCaster**)OFFSET_PHYSICSRAYQUERYVTABLE; } // offset never captured
};

class HavokPhysicsManager
{
public:
    PAD(0x30);
    HookCollisionInfo * m_collisions;
    HookProximityInfo * m_proximityObjects;
    void *m_havokManager;
    MaterialGridManager * m_materialGridManager;
    IPhysicsRayCaster * m_rayCaster;
};

class ClientLevel
{
public:
    PAD(0x9C);
    DebrisManager*    m_debrisManager;
    PAD(0x4);
    VegetationManager*    m_vegetationManager;
    PAD(0x4);
    EmitterManager*    m_emitterManager;
    void*    m_emitterRenderManager;
    void*    m_decalManager;
    PAD(16);
    WorldRenderModule* m_worldRenderModule;
    HavokPhysicsManager*  m_PhysicsManager;
    GameWorld* m_gameWorld;
    void* m_tweaker;
    BYTE m_isFinalized;
    BYTE m_hasStartedServer;
    BYTE m_autoRespawn;
    BYTE _ClientLevel_padding;
};

class ITypedObject
{
public:
    virtual TypeInfo * getType();
};

class DataContainer : public ITypedObject
{
public:
    WORD m_refCnt;
    WORD m_flags;
};

class GameDataContainer : public DataContainer
{
};

class GameObjectData : public GameDataContainer
{
public:
    WORD m_indexInBlueprint;
    BYTE m_isEventConnectionTarget;
    BYTE m_isPropertyConnectionTarget;
};

class EntityData : public GameObjectData
{
};

class SpatialEntityData : public EntityData
{
public:
    PAD(0x4);
    LinearTransform m_transform;
};

class GameEntityData : public SpatialEntityData
{
public:
    String m_name;
    BYTE m_enabled;
    BYTE m_runtimeComponentCount;
    PAD(0xA);
};

class GamePhysicsEntityData : public GameEntityData
{
public:
    DataContainer* m_physicsData;
};

class WeaponFiringData : public GameDataContainer
{
public:
    FiringFunctionData* m_primaryFire;
    FLOAT m_deployTime;
    FLOAT m_reactivateCooldownTime;
    FLOAT m_altDeployTime;
    PAD(0x4);
    INT m_shotLimit;
    DataContainer* m_weaponSway;
    FLOAT m_supportDelayProne;
};

class FiringFunctionData : public DataContainer
{
public:
    Array<FiringDispersionData> m_dispersion;
    SoldierWeaponDispersion m_weaponDispersion;
    Array<FireEffectData> m_fireEffects1p;
    Array<FireEffectData> m_fireEffects3p;
    DataContainer* m_sound;
    PAD(0x4);
    ShotConfigData m_shot;
    PAD(0x4);
    FireLogicData m_fireLogic;
    AmmoConfigData m_ammo;
    PAD(0x4);
    OverHeatData m_overHeat;
    PAD(0x4);
    FLOAT m_selfHealTimeWhenDeployed;
    FLOAT m_ammoCrateReloadDelay;
    CHAR m_unlimitedAmmoForAI;
    CHAR m_usePrimaryAmmo;
    PAD(0x2);
};

class ShotConfigData
{
public:
    Vec3 m_initialPosition;
    Vec3 m_initialDirection;
    Vec3 m_initialSpeed;
    FLOAT m_inheritWeaponSpeedAmount;
    DWORD m_muzzleExplosion;
    BulletEntityData* m_projectileData;
    BulletEntityData* m_secondaryProjectileData;
    DWORD m_projectile;
    DWORD m_secondaryProjectile;
    FLOAT m_spawnDelay;
    DWORD m_numberOfBulletsPerShell;
    DWORD m_numberOfBulletsPerShot;
    DWORD m_numberOfBulletsPerBurst;
    CHAR m_relativeTargetAiming;
    CHAR m_forceSpawnToCamera;
    CHAR m_spawnVisualAtWeaponBone;
    CHAR m_activeForceSpawnToCamera;
};

class ProjectileEntityData : public GamePhysicsEntityData
{
public:
    PAD(0xC);
    INT m_hitReactionWeaponType;
    FLOAT m_initialSpeed;
    FLOAT m_timeToLive;
    FLOAT m_initMeshHideTime;
    FLOAT m_visualConvergeDistance;
    FLOAT m_Bulletspeed;
    MaterialContainerPair* m_materialPair;
    DataContainer* m_explosion;
    WeaponSuppressionData* m_suppressionData;
    String m_ammunitionType;
    CHAR m_serverProjectileDisabled;
    CHAR m_detonateOnTimeout;
    PAD(0x2);
};

class BulletEntityData : public ProjectileEntityData
{
public:
    Vec3 m_initialAngularVelocity;
    void* m_trailEffect;
    void* m_mesh;
    float m_maxAttachableInclination;
    bool m_extraDamping;
    bool m_isAttachable;
    float m_stamina;
    void* m_flyBySound;
    void* m_dudExplosion;
    PAD(0x84);
    float m_gravity;
    PAD(0x30);
    float m_startDamage;
    float m_endDamage;
    float m_damageFalloffStartDistance;
    float m_damageFalloffEndDistance;
    float m_timeToArmExplosion;
    byte m_hasVehicleDetonation;
    byte m_instantHit;
};

class BoneCollisionComponent
{
public:
    class BoneTransformInfo
    {
    public:
        LinearTransform transform;
        Vec3 position;
    };

    BoneCollisionComponentData* m_boneCollisionData;
    ant::UpdatePoseResultData m_updatePoseResultData;
    ant::AnimationSkeleton* m_skeleton;
    BoneTransformInfo* m_boneCollisionTransforms;
    eastl::vector<eastl::pair<INT, MaterialContainerPair *>> m_boneCollisionInfo;
    FLOAT m_latencyBufferTime;
    FLOAT m_latencyBufferInterval;
    PAD(0x174);
    BoneTransformInfo* m_latencyTransforms;
    CHAR m_hiLod;
    PAD(0x3);
    DWORD m_debugColor;
    INT m_collisionBoneCount;
    byte m_collisionEnabled;
    byte m_collisionUpdated;
    CHAR m_isServer;
    PAD(0x1);
};

class ComponentCollection
{
public:
    GameEntity* owner;
    BYTE playerCount;
    BYTE totalCount;
    BYTE offsetCount;
    PAD(0x1);

    ComponentInfo *getInfo(int index)
    {
        ComponentInfo *info = (ComponentInfo *)this;
        return &(info[index + 1]);
    }
};

class Component : public EntityBusPeer
{
public:
    ComponentData* m_data;
    ComponentCollection* m_collection;
    PAD(0x4);
};

class ClientComponent : public Component
{
};

class ClientBoneCollisionComponent : public ClientComponent, public BoneCollisionComponent
{
};

class SoldierEntity // Inherited class at offset 0x100
{
public:
    enum DeathType
    {
        Shot,
        ShotInVehicleEntry,
        InsideExplodingVehicle,
        DeathTypeCount,
    };

    enum SoldierInteractedStatus
    {
        SoldierInteractedStatus_None,
        SoldierInteractedStatus_BeingInteracted,
        SoldierInteractedStatus_BeingInteractedCancelled,
        SoldierInteractedStatus_BeingInteractedFinished,
        SoldierInteractedStatus_Count,
    };

    virtual BoneCollisionComponent * boneCollisionComponent();
    virtual void *unknown1();
    virtual void *unknown2();
    virtual LinearTransform & soldierTransform();
    virtual void unknown4();
    virtual void unknown5();
    virtual void unknown6();
    virtual void unknown7();
    virtual bool isReloading();
    virtual void unknown9();
    virtual WeaponSway * getWeaponSway();
    virtual WeaponFiring * getCurrentWeaponFiring();
    virtual WeaponFiringData * getCurrentWeaponFiringData();
    virtual void unknown13();
    virtual void unknown14();
    virtual void unknown15();
    virtual int Pose();

    PAD(0x60);
    Component* m_space;
    SupportedShootingCallback* m_supportedShootingCallback;
    EyePositionCallback* m_eyePositionCallback;
    FLOAT m_maxHealth;
    CharacterPhysicsEntity* m_characterPhysicsentity;
    PAD(0x1C);
};

class ClientSoldierEntity : public ClientCharacterEntity, public SoldierEntity
{
public:
    class BreathControlHandler
    {
    public:
        BreathControlData* m_data;
        FLOAT m_breathControlTimer;
        FLOAT m_breathControlMultiplier;
        FLOAT m_breathControlPenaltyTimer;
        FLOAT m_breathControlPenaltyMultiplier;
        BYTE m_breathControlActive;
        PAD(0x3);
    };

    PAD(0x268);
    ClientSoldierPrediction* m_predictedController;
    ClientSoldierReplication* m_replicatedController;
    PAD(0xAC);
    LinearTransform m_meshTransform;
    SoldierEntityActionState m_oldActionState;
    CharacterPoseType m_previousPose;
    BYTE m_forceInivisble;
    BYTE m_derivedStatusUpdated;
    BYTE m_visualTransformUpdated;
    PAD(0x1);
    PAD(0x58);
    ClientSoldierWeaponsComponent* m_soldierWeaponComponent;
    PAD(0x4);
    ClientBoneCollisionComponent* m_boneCollisionComponent;
    PAD(0x8);
    BreathControlHandler* m_breathControlHandler;
    PAD(0xC);
    byte m_wasSprinting;
    byte m_isOccluded;

public:
    void UpdateAnimaTable()
    {
        if (POINTERCHK(m_animatableComponent[0]))
            m_animatableComponent[0]->m_hadVisualUpdate = true;
        if (POINTERCHK(m_animatableComponent[1]))
            m_animatableComponent[1]->m_hadVisualUpdate = true;
        m_visualTransformUpdated = 1;
    }

    ClientSoldierWeapon* GetCSW()
    {
        if (m_soldierWeaponComponent)
            if (m_soldierWeaponComponent->m_currentAnimatedWeaponHandler)
                return m_soldierWeaponComponent->m_currentAnimatedWeaponHandler->m_currentAnimatedWeapon;
        return NULL;
    }

    BOOL getPosition(Vec3* posBuffer)
    {
        if (m_replicatedController && posBuffer)
        {
            memcpy_s(posBuffer, sizeof(Vec3), &(m_replicatedController->m_state.position), sizeof(Vec3));
            return TRUE;
        }
        return FALSE;
    }

    BOOL GetVector(Vec3* posBuffer)
    {
        if (posBuffer)
        {
            memcpy_s(posBuffer, sizeof(fb::Vec3), &(this->m_meshTransform.trans), sizeof(fb::Vec3));
            return TRUE;
        }
        return FALSE;
    }
};

class Player
{
public:
    virtual void deconstructor();
    virtual SoldierEntity* getSoldier();
    virtual void dummy();
    virtual EntryComponent* getEntry();
    virtual bool isInVehicle();

    PlayerData* m_data;
    MemoryArena* m_arena;
    eastl::basic_string<char, eastl_arena_allocator> m_name;
    OnlineId m_onlineId;
    PAD(0x34C);
    int m_teamId;
};

class ClientPlayer : public Player
{
public:
    class ClientPlayerShooter : public WeaponFiringShooter
    {
    public:
        ClientPlayer* m_player;
    };

    PAD(0xa0);
    WeakPtr<ClientSoldierEntity> m_soldier;
    WeakPtr<ClientSoldierEntity> m_corpse;
    WeakPtr<ClientCharacterEntity> m_character;
    WeakTokenHolder m_weakTokenHolder;
    ClientControllableEntity* m_attachedControllable;
    PAD(0x04);
    ClientControllableEntity* m_controlledControllable;
    PAD(0x10);
    DWORD m_id;
    PAD(0x2c);

public:
    ClientSoldierEntity* getClientSoldier()
    {
        DWORD dwSoldier = (DWORD)this->getSoldier();
        if (dwSoldier)
            return (ClientSoldierEntity*)(dwSoldier - 0x100);
        return 0;
    }

    BOOL GetPlayerVector(Vec3* posBuffer)
    {
        if (posBuffer && POINTERCHK(getSoldier()))
        {
            memcpy_s(posBuffer, sizeof(fb::Vec3), &(this->getSoldier()->soldierTransform().trans), sizeof(fb::Vec3));
            return TRUE;
        }
        return FALSE;
    }

    Vec3 GetPlayerForward()
    {
        return getSoldier()->soldierTransform().forward;
    }
};

class ClientAntAnimatableComponent
{
public:
    BYTE pad_001[0x20];
    BYTE pad_002[0x110];
    bool m_hadVisualUpdate;
};

class CharacterEntity
{
public:
    virtual bool isAlive();
    virtual bool isDead();
    virtual bool isDying();
    virtual float mxHealth();
    virtual float yaw();
    virtual float pitch();
    virtual void function0();
    virtual bool isVisible();
    virtual float getHealth();
    virtual bool isAIPlayer();
    virtual void function1();
    virtual bool isSingleplayer();
    virtual void getPhysicsInfoForAnimation(LPVOID, LPVOID);
    virtual bool isInVehicle();
    virtual PhysicsEntityBase* physics();
    virtual CharacterPoseType pose();
    virtual PersonViewEnum activeView();
};

class ClientCharacterEntity : public ClientControllableEntity, public CharacterEntity
{
public:
    PAD(0x10);
    ClientPlayer* m_player;
    ClientAntAnimatableComponent* m_animatableComponent[2];
    PAD(0x30);
};

template <class T>
class GamePhysicsEntity : public T
{
public:
    FLOAT m_health;
};

class ClientPhysicsEntity : public GamePhysicsEntity<ClientGameEntity>, public IRigidBodyHook
{
};

template <class T>
class ClientGhostGameEntity : public T, public network::ClientGhost
{
};

class ClientGhostAndNetworkableGameEntity : public ClientGhostGameEntity<ClientPhysicsEntity>, public network::IClientNetworkable
{
};

class ControllableEntity
{
public:
    virtual float maxHealth();
    MaterialContainerPair* m_material;
    INT m_teamId;
    INT m_defaultTeamId;
};

class ClientControllableEntity : public ClientGhostAndNetworkableGameEntity, public ControllableEntity
{
public:
    eastl::vector<ClientEntryComponent*> m_entries;
    DWORD m_currentVelocityNormalizedOut;
    DWORD m_currentHealthNormalizedOut;
    float m_oldHealth;
    PAD(0x14);
};

#endif // #if 0
