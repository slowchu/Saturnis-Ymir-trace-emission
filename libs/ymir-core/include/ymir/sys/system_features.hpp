#pragma once

namespace ymir::sys {

struct SystemFeatures {
    bool enableDebugTracing = false;
    bool emulateSH2Cache = false;
    bool enableBusContention = false;
    bool enableIFMAContention = false;
    bool enableBusContentionStats = false;
    bool enableSCUDMAArbitration = true;
    bool enableSCUDMALocalArbiterTick = true;
    bool enableBBusSCSPArbitration = true;
    bool enableStrictSCUDMARestrictions = true;
};

} // namespace ymir::sys
