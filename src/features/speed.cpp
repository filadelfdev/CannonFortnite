#include "features/speed.h"
#include "sdk/offsets.h"
#include "core/memory.h"
#include "antiban/clamp.h"

namespace cf::feat {

SpeedCfg g_speedCfg{};

void TickSpeed(const LocalPlayerCache& lp, int tickIndex){
    if(!g_speedCfg.enabled.load()) return;
    if(!lp.movementComp) return;
    if(g_speedCfg.pulseWrite.load() &&
       (tickIndex % g_speedCfg.pulseEveryTicks.load()) != 0) return;

    // Server echoes a cap in CMC + a replicated version on the PlayerState.
    // We stay just under the hard ceiling so reconciliation never fires.
    float serverCap = cf::mem::R<float>(lp.movementComp + cf::off::CMC_MaxWalkSpeed);
    float walk  = cf::ab::clamp::speedCap(g_speedCfg.walkSpeed.load(),  serverCap*4.f);
    float fly   = cf::ab::clamp::speedCap(g_speedCfg.flySpeed.load(),   serverCap*6.f);

    cf::mem::Write(lp.movementComp + cf::off::CMC_MaxWalkSpeed,    &walk, sizeof(float));
    cf::mem::Write(lp.movementComp + cf::off::CMC_MaxFlySpeed,     &fly,  sizeof(float));

    float acc = g_speedCfg.acceleration.load();
    float air = g_speedCfg.airControl.load();
    float grv = g_speedCfg.gravityScale.load();
    float jmp = g_speedCfg.jumpZ.load();
    float brk = g_speedCfg.brakingFriction.load();

    cf::mem::Write(lp.movementComp + cf::off::CMC_MaxAcceleration,  &acc, sizeof(float));
    cf::mem::Write(lp.movementComp + cf::off::CMC_AirControl,       &air, sizeof(float));
    cf::mem::Write(lp.movementComp + cf::off::CMC_GravityScale,     &grv, sizeof(float));
    cf::mem::Write(lp.movementComp + cf::off::CMC_JumpZVelocity,    &jmp, sizeof(float));
    cf::mem::Write(lp.movementComp + cf::off::CMC_BrakingFriction,  &brk, sizeof(float));
}

} // namespace cf::feat
