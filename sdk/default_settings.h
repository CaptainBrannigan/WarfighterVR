#pragma once
// Built-in default settings (2026-10-16, the Beta release): the author's own mohwvr_settings.ini at release time --
// every setting and the weapon profiles tuned so far (not every weapon in the game has one yet: the campaign hasn't
// been played all the way through). sdk/settings.cpp LoadSettings: a first launch (no ini) starts from this text, and
// an existing ini picks up these weapon profiles for weapons it has none for. Same format as the ini.

namespace mohw {

inline constexpr const char kBuiltInSettings[] = R"MOHWVR(
IpdScale=1.0000
HeadAimEnabled=1
HeadAimSensitivity=1.0000
HeadAimInvertYaw=1
HeadAimInvertPitch=1
HeadAimClampPitch=1
HeadRollEnabled=1
HeadRollInvert=0
RotationSmoothingEnabled=1
RotationSmoothingWindowMs=34.0000
VrTurnSpeed=2.0000
VrStickFullDeflection=0.9000
TwoHandGrabRadius=0.1500
TwoHandReach=0.8000
HandOffsetRight=0.0000
HandOffsetPitch=-10.00
HandOffsetUp=0.0000
HandOffsetYaw=0.00
HandOffsetForward=0.0000
HandOffsetRoll=0.00
SecondaryGripRight=0.0000
SecondaryGripUp=0.0000
HudPlacementEnabled=1
HideReticle=1
HideBodyPart1=1
HideBodyPart2=1
HideBodyPart3=1
HudScale=0.5500
MenuScreenScale=0.7500
DrivingHeadLookInvert=0
DrivingHeadLookAuto=1
DrivingStickLookRange=90
DrivingStickDeadzone=0.50
DrivingStereo=1
DrivingGameThreadNextEye=0
DrivingSteeringWheel=1
DrivingWheelLock=190
DrivingWheelForward=0.45
DrivingWheelDown=0.20
DrivingWheelRight=0.00
DrivingWheelTilt=10
DrivingWheelRadius=0.10
DrivingWheelThickness=0.030
DrivingWheelOpacity=0.15
DebugHideDraw=0
HudIpdScale=0.2000
HudDepth=0.1000
HudOffsetX=0.0000
HudOffsetY=-10.5000
RemoveShotSpread=1
ControllerAimUp=0.00
ControllerAimRight=0.00
AdsSwayScale=0.00
FireViaMouse=0
LeftHanded=0
WeaponDriveEnabled=1
HolsterHoldToKeep=0
HolsterRadius=0.3000
HolsterDrop=0.0000
HolsterStomach=ConceptSelectInventoryItem1
HolsterHip=ConceptSelectInventoryItem2
HolsterChest=ConceptThrowGrenade
HolsterLeftShoulder=ConceptMeleeAttack
HolsterRightShoulder=ConceptMeleeAttack
HolsterStomachPos=0.1000,-0.5500,0.1200
HolsterHipPos=0.2200,-0.7800,0.0000
HolsterChestPos=0.0000,-0.3300,0.1400
HolsterLeftShoulderPos=-0.2000,-0.1000,-0.1000
HolsterRightShoulderPos=0.2000,-0.1000,-0.1000
SightDotEnabled=1
DotsOnlyWithGrip=1
GripDoubleTapBumpers=1
SightZeroDistance=1000.0000
SightDotSize=0.1500
OpticDotEnabled=0
OpticHeight=0.2060
OpticWindageOffset=-0.0260
OpticDistance=0.0400
OpticDotGreen=0
RayDotGreen=0
LaserBeam=1
LaserBeamOpacity=0.35
LaserBeamWidth=3.0
LaserRaycastEveryFrames=1
LaserStart=0.150
LaserBeamStartRight=0.000
LaserBeamStartUp=0.000
WeaponGripRight=0.0900
WeaponGripUp=-0.2100
WeaponGripBack=-0.2400
WeaponGripPitch=0.0000
WeaponGripYaw=0.0000
WeaponGripRoll=-10.0000
WeaponProfile=MOHW/Weapons/ASSAULT/AK103/AK103|0.0750|-0.1900|-0.1900|0.0000|0.0000|0.0000|0.2060|-0.0260|0.0400|0.0000|0.0000|-0.0750|0.0000|0.1500|0.0000|0.0000
WeaponProfile=MOHW/Weapons/ASSAULT/AK103/spec/AK103__AKM|0.0800|-0.1600|-0.2300|0.0000|0.0000|0.0000|0.2060|-0.0260|0.0400|0.0000|0.0000|-0.0500|0.0000|0.1500|0.0000|0.0000
WeaponProfile=MOHW/Weapons/ASSAULT/OBR_556/OBR_556|0.0750|-0.1400|-0.2400|0.0000|0.0000|1.0000|0.1580|0.0000|0.0400|0.0000|0.0000|-0.0150|0.0000|0.4500|0.0550|0.0800
WeaponProfile=MOHW/Weapons/ASSAULT/SUCHKA/SUCHKA|0.0500|-0.1800|-0.2400|0.0000|0.0000|0.0000|0.2060|-0.0260|0.0400|0.0000|0.0000|-0.0500|0.0000|0.1500|0.0000|0.0000
WeaponProfile=MOHW/Weapons/LMG/PKP/PKP|0.0900|-0.2100|-0.2400|0.0000|0.0000|5.0000|0.2050|-0.0260|0.0400|0.0000|0.0000|-0.0500|0.0000|0.7500|0.0000|0.0000
WeaponProfile=MOHW/Weapons/PISTOL/HK45C/spec/HK45C__SP|0.0950|-0.0950|-0.3400|-23.0000|0.0000|0.0000|0.2060|-0.0260|0.0400|0.0000|0.0000|0.0000|0.0000|0.1500|0.0000|0.0000
WeaponProfile=MOHW/Weapons/SMG/MP7/MP7|0.0200|-0.2000|-0.3000|10.0000|0.0000|0.0000|0.2060|-0.0260|0.0400|0.0000|0.0000|-0.0100|0.0600|0.1500|0.0000|0.0000
WeaponProfile=MOHW/Weapons/SNIPER/TAC300/TAC300|0.0050|-0.1600|-0.1900|0.0000|0.0000|0.0000|0.2060|-0.0260|0.0400|0.0000|0.0000|-0.0250|0.0000|0.1500|0.0000|0.0000
)MOHWVR";

} // namespace mohw
