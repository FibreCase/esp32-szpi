#include <assert.h>
#include <math.h>
#include <stdint.h>
#include "../components/szpi_input/src/szpi_boot_state.h"
#include "../components/szpi_input/src/szpi_imu_math.h"

static void test_boot_debounce_and_clicks(void)
{
    szpi_boot_debouncer_t state;
    szpi_boot_debouncer_seed(&state, false, 0);
    assert(szpi_boot_debouncer_step(&state, true, 1) == 0);
    assert(szpi_boot_debouncer_step(&state, false, 8) == 0);
    assert(szpi_boot_debouncer_step(&state, true, 12) == 0);
    assert(szpi_boot_debouncer_step(&state, true, 31) == 0);
    assert(szpi_boot_debouncer_step(&state, true, 32) == SZPI_BOOT_STATE_EVENT_PRESSED);
    assert(szpi_boot_debouncer_step(&state, true, 831) == 0);
    assert(szpi_boot_debouncer_step(&state, true, 832) == SZPI_BOOT_STATE_EVENT_LONG_PRESS);
    assert(szpi_boot_debouncer_step(&state, true, 2000) == 0);
    assert(szpi_boot_debouncer_step(&state, false, 2001) == 0);
    assert(szpi_boot_debouncer_step(&state, false, 2021) ==
           (SZPI_BOOT_STATE_EVENT_RELEASED));

    szpi_boot_debouncer_seed(&state, false, 100);
    assert(szpi_boot_debouncer_step(&state, true, 101) == 0);
    assert(szpi_boot_debouncer_step(&state, true, 121) == SZPI_BOOT_STATE_EVENT_PRESSED);
    assert(szpi_boot_debouncer_step(&state, false, 122) == 0);
    assert(szpi_boot_debouncer_step(&state, false, 142) ==
           (SZPI_BOOT_STATE_EVENT_RELEASED | SZPI_BOOT_STATE_EVENT_SHORT_PRESS));
}

static void test_boot_held_at_start_and_clock_wrap(void)
{
    szpi_boot_debouncer_t state;
    szpi_boot_debouncer_seed(&state, true, UINT32_MAX - 10U);
    assert(szpi_boot_debouncer_step(&state, true, UINT32_MAX - 1U) == 0);
    assert(szpi_boot_debouncer_step(&state, false, UINT32_MAX) == 0);
    assert(szpi_boot_debouncer_step(&state, false, 19U) == 0);
    assert(state.armed && !state.stable_pressed);
    assert(szpi_boot_debouncer_step(&state, true, 20U) == 0);
    assert(szpi_boot_debouncer_step(&state, true, 40U) == SZPI_BOOT_STATE_EVENT_PRESSED);
}

static void test_imu_conversion_and_tilt(void)
{
    assert(szpi_imu_decode_i16_le(0x00, 0x80) == INT16_MIN);
    assert(szpi_imu_decode_i16_le(0xFF, 0x7F) == INT16_MAX);
    assert(szpi_imu_decode_i16_le(0x00, 0xFF) == -256);
    assert(fabsf(szpi_imu_accel_to_g(8192) - 1.0f) < 0.0001f);
    assert(fabsf(szpi_imu_gyro_to_dps(-64) + 1.0f) < 0.0001f);

    float roll = 0.0f, pitch = 0.0f;
    const float level[] = {0.0f, 0.0f, 1.0f};
    assert(szpi_imu_compute_tilt(level, &roll, &pitch));
    assert(fabsf(roll) < 0.001f && fabsf(pitch) < 0.001f);
    const float tilted[] = {0.0f, 1.0f, 0.0f};
    assert(szpi_imu_compute_tilt(tilted, &roll, &pitch));
    assert(fabsf(roll - 90.0f) < 0.001f);
    const float freefall[] = {0.0f, 0.0f, 0.0f};
    const float dynamic[] = {1.0f, 1.0f, 1.0f};
    assert(!szpi_imu_compute_tilt(freefall, &roll, &pitch));
    assert(!szpi_imu_compute_tilt(dynamic, &roll, &pitch));
}

int main(void)
{
    test_boot_debounce_and_clicks();
    test_boot_held_at_start_and_clock_wrap();
    test_imu_conversion_and_tilt();
    return 0;
}
