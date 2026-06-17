#pragma once

#include <cstdint>

namespace Monitor {
    enum class eDPMSAction : uint8_t {
        NONE = 0,
        COMMIT,
        CYCLE,
    };

    eDPMSAction dpmsActionFor(bool requestedOn, bool logicalOn, bool outputEnabled, bool wakePending);
    bool        shouldApplyDPMSState(bool requestedOn, bool logicalOn, bool outputEnabled, bool wakePending);
}
