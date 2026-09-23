/*
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef IMU_H
#define IMU_H

/* Includes */
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * BMI270 6-axis IMU.
 *
 * The sensor is configured in imu.c with the accelerometer at +/-4 g and the
 * gyroscope at +/-500 dps, which fixes the two conversion factors below.
 */
#define IMU_ACC_LSB_PER_G   (8192.0f)  /* 2^15 / 4   */
#define IMU_GYR_LSB_PER_DPS (65.536f)  /* 2^15 / 500 */

/* One sample, already converted out of raw counts. */
typedef struct {
    float acc[3];  /* g   */
    float gyr[3];  /* dps */
} imu_sample_t;

/*
 * Attitude in degrees.
 *
 * All three angles come out of a quaternion attitude filter - the filter's
 * internal state is a quaternion and the Euler conversion happens once, at the
 * last step, in imu_read_attitude(). See docs/imu.md section 4.
 *
 * roll and pitch are absolute. Left alone the accelerometer fixes both against
 * gravity, so they stay correct while the board is being moved, not just while
 * it is still. Note that the Euler representation itself degenerates as pitch
 * approaches +/-90 degrees; anything that needs the attitude there wants the
 * quaternion, which is not in the BLE payload.
 *
 * yaw is NOT absolute. The BMI270 has no magnetometer, so there is no heading
 * reference to observe; a 6-axis part simply cannot measure heading. Nothing in
 * the filter corrects yaw - no accelerometer reading can see it, because a
 * rotation about gravity leaves the accelerometer output unchanged. What is
 * reported here is the gyroscope Z rate integrated over time, zeroed once at
 * boot after a still-period bias calibration and then tracked continuously by
 * FusionBias. It is a relative turn angle: usable to see "how far has this been
 * rotated since power-on", correct over seconds to minutes, but it drifts and
 * is not a compass heading. Do not feed it into anything that needs an absolute
 * bearing.
 */
typedef struct {
    float roll;   /* deg, -180 .. +180 */
    float pitch;  /* deg,  -90 ..  +90 */
    float yaw;    /* deg, relative to boot, drifts */
} imu_attitude_t;

/*
 * @brief Create the I2C bus and bring up the BMI270.
 *
 * Loads the Bosch toy configuration firmware, enables the accelerometer and
 * gyroscope, then runs a short gyroscope-Z bias calibration. The board must be
 * still for roughly the first 0.5 s after this call, otherwise the yaw
 * reference picks up the movement as bias.
 *
 * @return ESP_OK on success.
 */
esp_err_t imu_init(void);

/*
 * @brief Read a fresh sample from the sensor.
 *
 * @param out Destination for the sample. Must not be NULL.
 * @return ESP_OK on success.
 */
esp_err_t imu_read(imu_sample_t *out);

/*
 * @brief Route the BMI270 data-ready signal to INT1 / CONFIG_IMU_INT_GPIO.
 *
 * Call after imu_init() and after the task that will wait for samples exists.
 * Each completed conversion pulses the line and wakes @p waiter with a task
 * notification, so sampling is paced by the sensor rather than by a timer.
 *
 * This is an optimisation, not a prerequisite: the IMU task waits with a
 * timeout, so if the line is missing or this call fails, sampling degrades to
 * CONFIG_IMU_NOTIFY_PERIOD_MS instead of stopping.
 *
 * @param waiter Task to notify on each data-ready edge. Must not be NULL.
 * @return ESP_OK on success.
 */
esp_err_t imu_start_data_ready(TaskHandle_t waiter);

/*
 * @brief Read a fresh sample and turn it into an attitude.
 *
 * This is the function the IMU task calls. It advances the yaw integration,
 * so it should be called at a steady rate.
 *
 * @param out Destination for the attitude. Must not be NULL.
 * @return ESP_OK on success.
 */
esp_err_t imu_read_attitude(imu_attitude_t *out);

/*
 * @brief Copy the most recent attitude produced by imu_read_attitude().
 *
 * Used by the GATT read callback, so that the value a notification carries is
 * exactly the one that was logged.
 *
 * @param out Destination for the attitude. Must not be NULL.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if no attitude has been computed.
 */
esp_err_t imu_get_latest_attitude(imu_attitude_t *out);

/*
 * @brief Copy the raw sample that produced the latest attitude.
 *
 * Exists so a caller can compare the fused attitude against what the plain
 * accelerometer formulas would have said about the *same* reading - which is
 * how the dynamic-accuracy test demonstrates acceleration rejection.
 *
 * @param out Destination for the sample. Must not be NULL.
 * @return ESP_OK, or ESP_ERR_INVALID_STATE if no sample has been read yet.
 */
esp_err_t imu_get_latest_sample(imu_sample_t *out);

/*
 * @brief Number of data-ready edges seen since boot.
 *
 * This exists because a dead INT1 line is invisible from the outside: the IMU
 * task keeps sampling on its timeout either way, so the only symptom would be
 * a lower sample rate and a looser yaw integral, with no error anywhere.
 * Read it a second or so after imu_start_data_ready(); at the configured
 * 200 Hz output data rate expect roughly 200.
 *
 * @return Edge count.
 */
uint32_t imu_get_data_ready_count(void);

/*
 * @brief Number of samples rejected by the plausibility gate since boot.
 *
 * A non-zero, growing count means the sensor or the I2C link is producing
 * corrupted readings. Those samples never reach the filter, so the attitude
 * stays clean - but the cause is worth chasing, and this counter is the only
 * evidence that it is happening at all.
 *
 * @return Rejected sample count.
 */
uint32_t imu_get_discarded_sample_count(void);

#endif // IMU_H
