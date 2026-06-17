#include "DPMS.hpp"

Monitor::eDPMSAction Monitor::dpmsActionFor(bool requestedOn, bool logicalOn, bool outputEnabled, bool wakePending) {
    if (logicalOn != requestedOn)
        return eDPMSAction::COMMIT;

    if (!requestedOn)
        return eDPMSAction::NONE;

    if (!outputEnabled)
        return eDPMSAction::COMMIT;

    if (wakePending)
        return eDPMSAction::CYCLE;

    return eDPMSAction::NONE;
}

bool Monitor::shouldApplyDPMSState(bool requestedOn, bool logicalOn, bool outputEnabled, bool wakePending) {
    return dpmsActionFor(requestedOn, logicalOn, outputEnabled, wakePending) != eDPMSAction::NONE;
}
