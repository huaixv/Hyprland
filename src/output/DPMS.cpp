#include "DPMS.hpp"

bool Monitor::shouldApplyDPMSState(bool requestedOn, bool logicalOn, bool outputEnabled) {
    if (logicalOn != requestedOn)
        return true;

    return requestedOn && !outputEnabled;
}
