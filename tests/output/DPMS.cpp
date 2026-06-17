#include <output/DPMS.hpp>

#include <gtest/gtest.h>

TEST(DPMS, appliesWhenLogicalOnButOutputDisabled) {
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(true, true, false));
}

TEST(DPMS, skipsWhenLogicalAndOutputStateAlreadyMatch) {
    EXPECT_FALSE(Monitor::shouldApplyDPMSState(true, true, true));
    EXPECT_FALSE(Monitor::shouldApplyDPMSState(false, false, false));
}

TEST(DPMS, appliesWhenLogicalStateChanges) {
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(true, false, false));
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(false, true, true));
}
