#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SZPI_IMU_ACCEL_LSB_PER_G 8192.0f
#define SZPI_IMU_GYRO_LSB_PER_DPS 64.0f
#define SZPI_IMU_GRAVITY_MIN_G 0.75f
#define SZPI_IMU_GRAVITY_MAX_G 1.25f
#define SZPI_IMU_RAD_TO_DEG 57.29577951308232f

static inline int16_t szpi_imu_decode_i16_le(uint8_t low, uint8_t high)
{
    uint16_t bits = (uint16_t)low | ((uint16_t)high << 8);
    return bits < 0x8000U ? (int16_t)bits : (int16_t)((int32_t)bits - 65536);
}

static inline float szpi_imu_accel_to_g(int16_t raw)
{
    return (float)raw / SZPI_IMU_ACCEL_LSB_PER_G;
}

static inline float szpi_imu_gyro_to_dps(int16_t raw)
{
    return (float)raw / SZPI_IMU_GYRO_LSB_PER_DPS;
}

static inline bool szpi_imu_compute_tilt(const float accel_g[3], float *roll_deg,
                                        float *pitch_deg)
{
    if (accel_g == NULL || roll_deg == NULL || pitch_deg == NULL) return false;
    float magnitude = sqrtf(accel_g[0] * accel_g[0] + accel_g[1] * accel_g[1] +
                            accel_g[2] * accel_g[2]);
    if (!isfinite(magnitude) || magnitude < SZPI_IMU_GRAVITY_MIN_G ||
        magnitude > SZPI_IMU_GRAVITY_MAX_G) return false;
    *roll_deg = atan2f(accel_g[1], accel_g[2]) * SZPI_IMU_RAD_TO_DEG;
    *pitch_deg = atan2f(-accel_g[0], sqrtf(accel_g[1] * accel_g[1] +
                                           accel_g[2] * accel_g[2])) * SZPI_IMU_RAD_TO_DEG;
    return isfinite(*roll_deg) && isfinite(*pitch_deg);
}
