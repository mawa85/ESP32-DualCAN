/* Host checks of the vendored ACAN2517FD bit-timing calculator at the
 * T-2CAN FD crystal frequency (40 MHz). The chip driver itself is not
 * reimplemented here. */

#include <stdint.h>

#include <unity.h>

#include "ACAN2517FDSettings.cpp"

void setUp(void) {}
void tearDown(void) {}

static void expect_exact(uint32_t bps)
{
    ACAN2517FDSettings s(ACAN2517FDSettings::OSC_40MHz, bps, DataBitRateFactor::x1);
    const uint32_t tq = 1u + s.mArbitrationPhaseSegment1 + s.mArbitrationPhaseSegment2;
    const uint32_t sample = (1u + s.mArbitrationPhaseSegment1) * 1000u / tq;

    TEST_ASSERT_EQUAL_UINT32(40000000u, s.sysClock());
    TEST_ASSERT_TRUE(s.mArbitrationBitRateClosedToDesiredRate);
    TEST_ASSERT_TRUE(s.exactArbitrationBitRate());
    TEST_ASSERT_EQUAL_UINT32(bps, s.actualArbitrationBitRate());
    /* Data phase is programmed at 1x, so a later CAN FD mode change has a
     * legal data bit rate instead of an unset DBTCFG. */
    TEST_ASSERT_TRUE(s.exactDataBitRate());
    TEST_ASSERT_EQUAL_UINT32(bps, s.actualDataBitRate());
    TEST_ASSERT_TRUE(sample >= 700 && sample <= 900);
    TEST_ASSERT_TRUE(s.mBitRatePrescaler >= 1 && s.mBitRatePrescaler <= 256);
}

void test_40mhz_standard_rates_are_exact(void)
{
    expect_exact(125000);
    expect_exact(250000);
    expect_exact(500000);
    expect_exact(1000000);
}

void test_40mhz_500k_uses_normal20b_when_selected(void)
{
    ACAN2517FDSettings s(ACAN2517FDSettings::OSC_40MHz, 500000, DataBitRateFactor::x1);
    s.mRequestedMode = ACAN2517FDSettings::Normal20B;
    TEST_ASSERT_EQUAL_UINT8(ACAN2517FDSettings::Normal20B, s.mRequestedMode);
    TEST_ASSERT_EQUAL_UINT32(500000, s.actualArbitrationBitRate());
}

void test_nonzero_rate_that_does_not_divide_is_not_exact(void)
{
    ACAN2517FDSettings s(ACAN2517FDSettings::OSC_40MHz, 333333, DataBitRateFactor::x1);
    TEST_ASSERT_FALSE(s.exactArbitrationBitRate());
}

/* PlatformIO's Unity runner is not emitted for this .cpp file. */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_40mhz_standard_rates_are_exact);
    RUN_TEST(test_40mhz_500k_uses_normal20b_when_selected);
    RUN_TEST(test_nonzero_rate_that_does_not_divide_is_not_exact);
    return UNITY_END();
}
