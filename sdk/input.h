#pragma once
// Input node. Not needed for v1 (head tracking layers onto the camera
// transform in the RenderView hook, not through here), but preFrameUpdate
// (vtable slot 28) and m_motionController are the eventual hook point for
// routing VR controller poses into the game once motion-controller aiming
// becomes in scope.

#include "mohw_common.h"
#include "mohw_offsets.h"

namespace mohw {

class InputCache;
class Pad;
class MotionController;
class IKeyboard;
class IMouse;

class BorderInputNode
{
public:
    virtual void init();
    virtual void Function1();
    virtual void Function2();
    virtual void Function3();
    virtual void Function4();
    virtual void Function5();
    virtual void Function6();
    virtual void Function7();
    virtual void Function8();
    virtual void Function9();
    virtual void Function10();
    virtual void Function11();
    virtual void Function12();
    virtual void Function13();
    virtual void Function14();
    virtual void Function15();
    virtual void Function16();
    virtual void Function17();
    virtual void Function18();
    virtual void Function19();
    virtual void Function20();
    virtual void Function21();
    virtual void Function22();
    virtual void Function23();
    virtual void Function24();
    virtual void Function25();
    virtual void Function26();
    virtual void Function27();
    virtual void preFrameUpdate(float); // vtable slot 28
    virtual void Function29();

    PAD(0x4);
    InputCache* m_inputCache;
    byte m_forceReadCache; // 0x9
    PAD(0x3);
    Pad* m_pad;                           // 0xC
    MotionController* m_motionController; // 0x10
    IKeyboard* m_keyboard;                // 0x14
    IMouse* m_mouse;                      // 0x18
    byte m_disableInput;                  // 0x1C

public:
    static BorderInputNode* Singleton() { return *Offset<BorderInputNode**>(OFFSET_BORDERINPUTNODE); }
};

class InputCache
{
public:
    byte m_disableCache; // 0x0
    PAD(0x3);
    float flInputBuffer[123]; // 0x4
};

} // namespace mohw
