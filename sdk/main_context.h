#pragma once
// Top-level game/client singletons. Not on the critical path for v1
// (head-tracked stereo only needs DxRenderer + RenderView/GameRenderer), but
// kept resolvable now since later phases (motion controllers, player-relative
// scale) will need ClientGameContext -> ClientLevel -> local player.
//
// Several member types here (Client, PlayerData, GameTime, ClientLevel,
// MaterialGridManager, ClientPlayerManager) haven't been reversed yet and are
// only ever used as opaque pointers below -- forward-declared, not defined.

#include "mohw_common.h"
#include "mohw_offsets.h"

namespace mohw {

class Client;
class PlayerData;
class GameTime;
class ClientLevel;
class MaterialGridManager;
class ClientPlayerManager;
class Player;

template <typename T>
class EastlVectorStub; // placeholder: real code uses eastl::vector<T>; not needed until player enumeration is implemented

class Main
{
public:
    PAD(824);
    DWORD m_strMain; // 0x0338
    PAD(104);        // 0x033C
    Client* m_client; // 0x03A4
    char _0x03A8[48];

public:
    static Main* Singleton() { return *Offset<Main**>(OFFSET_MAIN); }
}; // Size=0x03D8

class PlayerManager
{
public:
    virtual void Function0();
    // virtual eastl::vector<Player*> getPlayers();
    // virtual eastl::vector<Player*> getSpectators();

    PlayerData* m_playerData;      // 0x04
    DWORD m_maxPlayerCount;        // 0x08
    DWORD m_playerCountBitCount;   // 0x0C
    DWORD m_playerIdBitCount;      // 0x10
}; // 0x14

class GameContext
{
public:
    PAD(8);                                       // 0x00
    PlayerManager* m_playerManager;               // 0x08
    GameTime* m_gameTime;                          // 0x0C
    ClientLevel* m_level;                          // 0x10
    MaterialGridManager* m_materialGridManager;    // 0x14
    DWORD m_animationManager;                      // 0x18
    DWORD m_modelAnimationManager;                 // 0x1C
    DWORD m_blueprintBundleManager;                // 0x20
    DWORD m_dlcManager;                            // 0x24
    DWORD m_demoControl;                           // 0x28
    int m_realm;                                    // 0x2C
}; // 0x30

class ClientGameContext
{
public:
    PAD(8);
    ClientPlayerManager* m_clientPlayerManager; // 0x0008
    GameTime* m_gameTime;                        // 0x000C
    ClientLevel* m_clientLevel;                  // 0x0010
    PAD(44);

public:
    static ClientGameContext* Singleton() { return *Offset<ClientGameContext**>(OFFSET_CLIENTGAMECONTEXT); }
}; // 0x34

} // namespace mohw
