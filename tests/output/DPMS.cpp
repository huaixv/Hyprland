#include <output/DPMS.hpp>

#include <gtest/gtest.h>

TEST(DPMS, appliesWhenLogicalOnButOutputDisabled) {
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(true, true, false, false));
}

TEST(DPMS, appliesWhenWakeIsStillPending) {
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(true, true, true, true));
    EXPECT_EQ(Monitor::dpmsActionFor(true, true, true, true), Monitor::eDPMSAction::CYCLE);
}

TEST(DPMS, skipsWhenLogicalAndOutputStateAlreadyMatch) {
    EXPECT_FALSE(Monitor::shouldApplyDPMSState(true, true, true, false));
    EXPECT_FALSE(Monitor::shouldApplyDPMSState(false, false, false, false));
}

TEST(DPMS, appliesWhenLogicalStateChanges) {
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(true, false, false, false));
    EXPECT_TRUE(Monitor::shouldApplyDPMSState(false, true, true, false));
}
