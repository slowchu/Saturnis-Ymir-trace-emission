#pragma once

#include <filesystem>

namespace app {

struct CommandLineOptions {
    std::filesystem::path gameDiscPath;
    std::filesystem::path profilePath;
    bool forceUserProfile;
    bool fullScreen;
    bool startPaused;
    bool startFastForward;
    bool enableDebugTracing;
    bool enableBusContention;
    bool enableIFMAContention;
    bool busContentionStats;
    bool busContentionSH2Only;
    bool busContentionSCULocalTick;
    bool busContentionNoSCSP;
    bool scuDMALenient;
};

} // namespace app
