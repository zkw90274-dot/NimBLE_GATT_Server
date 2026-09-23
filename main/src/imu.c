/*
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "imu.h"
#include "common.h"

#include <math.h>

/*
 * Do NOT include <driver/i2c.h> here. i2c_bus.h picks the right driver header
 * for the IDF version in use and carries its own definition of i2c_config_t;
 * pulling in the legacy header as well makes the two declarations clash.
 */
#include "i2c_bus.h"
#include "bmi270_api.h"
#include "esp_timer.h"

/*
 * driver/gpio.h is safe to pull in next to i2c_bus.h - the clash documented
 * above is specific to the legacy driver/i2c.h.
 */
#include "driver/gpio.h"

/*
 * Vendored Fusion AHRS - provenance and licence are documented in
 * fusion/README.md. The variant used here is FusionAhrsUpdateNoMagnetometer():
 * this board has no magnetometer, so heading stays unobservable.
 */
#include "FusionAhrs.h"
#include "FusionBias.h"
#include "FusionMath.h"

/* Private defines */
#define IMU_I2C_PORT I2C_NUM_0

/*
 * Fusion AHRS settings.
 *
 * IMU_ODR_HZ must match the ODR configured in imu_configure(). Fusion derives
 * its rejection timeout in *samples* from this, and its startup-gain ramp in
 * seconds, so a wrong value silently changes both.
 *
 * IMU_AHRS_GAIN is used as-is (Fusion applies it as
 * q += q * (gyro + gain * feedback) * dt, so the convergence time constant is
 * 1/gain seconds and does not scale with sample rate). 0.5 is the library
 * default, but the first dynamic test showed a slow ~10 s, +/-0.8 degree
 * wander in pitch while the board sat still - the classic sign of the loop
 * trusting the accelerometer too much. Lowered to 0.2 to trade tilt-response
 * speed for a calmer resting angle; see the dynamic test for the comparison.
 *
 * Measured on the host with the same source files and a 5 deg accelerometer
 * step (2026-09-23): gain 0.2 settles 63% of the step in 5.0 s, i.e. exactly
 * 1/gain. That measurement is the yardstick any future gain change has to beat.
 */
#define IMU_ODR_HZ 200.0f
#define IMU_AHRS_GAIN 0.2f
#define IMU_GYRO_RANGE_DPS 500.0f

/*
 * Fusion's own gyroscope over-range detection is deliberately switched off by
 * feeding it a range of 0. Its recovery path is far more expensive than the
 * condition it guards against, and the measurement below is why.
 *
 * When any axis passes 0.98 * gyroscopeRange, Fusion calls SoftRestart(), which
 * sets startup = true. That does two things for the next 3 seconds (the
 * STARTUP_PERIOD ramp, FusionAhrs.c:20-25):
 *   - the filter gain jumps from the tuned 0.2 to INITIAL_STARTUP_GAIN = 10.0,
 *     a factor of 50;
 *   - HalfInclinationFeedback() stops consulting accelerationRejection at all
 *     (FusionAhrs.c:243), so the accelerometer is believed unconditionally.
 *
 * Hand shaking saturates this sensor's +/-500 dps range routinely, so the
 * trigger fires exactly when the accelerometer is least trustworthy. Measured
 * on the host, one 550 dps sample followed by an accelerometer that claims a
 * 30 degree roll error (rejection threshold 10 degrees):
 *
 *     gyroscopeRange = 500 (detection on)  -> roll reaches the full +30.000
 *     gyroscopeRange = 0   (detection off) -> roll stays at +0.000
 *
 * The sample is not corrupted, it is a real over-range reading, so the
 * plausibility gate below does not catch it either - the gyroscope genuinely
 * read 550 dps, it just could not read more. The lost rotation during
 * saturation is unavoidable at this range; the 3 second loss of acceleration
 * rejection afterwards is not, and it is the part that does the damage.
 *
 * The cost of switching it off: after a saturation event the filter keeps
 * integrating rather than restarting. Since the gate only rejects readings
 * above IMU_GYR_MAX_DPS, a saturated (clipped) sample still gets integrated,
 * which under-counts that rotation either way.
 *
 * A future improvement would be to move the gyroscope to its +/-1000 dps range
 * during high-dynamics phases, which raises the clipping point instead of
 * papering over it - at the cost of halving the LSB resolution, so it needs its
 * own dynamic test before being adopted.
 */
#define IMU_ACC_REJECTION_DEG 10.0f
#define IMU_ACC_REJECTION_TIMEOUT_S 5.0f

/*
 * Runtime gyroscope bias tracking: only update the estimate while the measured
 * rate stays under the threshold, and only once it has done so continuously for
 * the given period.
 */
#define IMU_BIAS_STATIONARY_DPS 2.0f
#define IMU_BIAS_STATIONARY_S 5.0f

/*
 * Plausibility gate in front of the AHRS.
 *
 * One corrupted sample is enough to wreck the attitude: yaw has no absolute
 * reference, so a single bad step is integrated and never corrected. Testing
 * captured exactly that - the accelerometer read jumped for one 200 ms trace
 * interval (implying 5500 deg/s, far beyond the configured +/-500 dps range)
 * and yaw took an 18 degree step it never recovered from.
 *
 * The counters and the logged values matter as much as the guard does: without
 * them a glitch is indistinguishable from real motion.
 *
 * Thresholds are set at "physically impossible", not at "unusual". The first
 * version used a +/-2 g window, which threw away a quarter of the samples
 * during the shake phase - hand shaking reaches 2-3 g and passes through
 * near-free-fall, so those readings were real and the filter was being starved
 * exactly when it was most needed.
 *
 * The accelerometer range is +/-4 g per axis, so all three axes saturated is
 * 6.9 g; the gyroscope cannot read past its +/-500 dps range at all. Only the
 * gyroscope check is a genuine impossibility test.
 */
#define IMU_ACC_MIN_G 0.1f
#define IMU_ACC_MAX_G 7.0f
#define IMU_GYR_MAX_DPS (IMU_GYRO_RANGE_DPS * 1.02f)
#define IMU_BAD_SAMPLE_LOG_PERIOD_US 1000000

/*
 * Bring-up retry. The BMI270 occasionally does not answer I2C on the first
 * attempt after a power-on reset; see the comment at the call site.
 */
#define IMU_INIT_ATTEMPTS 3
#define IMU_INIT_RETRY_MS 100

/*
 * Sampling is paced by the BMI270 data-ready signal rather than by a timer:
 * the sensor pulses INT1 (CONFIG_IMU_INT_GPIO, GPIO5 on this board) once per
 * conversion and the ISR wakes the IMU task. The sensor free-runs at 200 Hz,
 * far more than BLE can carry, so the task rate-limits the notifications
 * itself - sampling fast keeps the yaw integral tight, the radio does not have
 * to keep up.
 *
 * INT1 is configured non-latched (a pulse per sample, matching the ESP-Spot
 * Lite treatment of the data-ready signal) and the GPIO edge is therefore the
 * rising one.
 *
 * The ISR only notifies a task: no I2C, no logging, nothing that can block.
 */

/*
 * Gyroscope Z bias calibration. Taken once at boot; the board has to be still
 * for this to mean anything, so the window is kept short.
 */
#define IMU_GYRO_BIAS_SAMPLES 64
#define IMU_GYRO_BIAS_INTERVAL_MS 5 /* 64 x 5 ms is about 0.3 s */
#define IMU_GYRO_BIAS_WARN_DPS 5.0f /* above this the reading is suspect */

/* Private variables */
static i2c_bus_handle_t s_i2c_bus;
static bmi270_handle_t s_handle;

/* Task woken by every data-ready edge. Written once, before the GPIO handler
 * is added, so the ISR can never see it half-set. */
static TaskHandle_t s_drdy_waiter;

/* Edges seen on the data-ready line, for the startup health check. A 32-bit
 * aligned load/store is atomic on this core, so no lock is needed. */
static volatile uint32_t s_drdy_count;

/* Samples thrown away by the plausibility gate, and the last time we said so.
 * Both only touched by the task calling imu_read_attitude(). */
static uint32_t s_discarded_samples;
static int64_t s_last_bad_log_us;

/*
 * Latest attitude, written by the IMU task and read by the NimBLE host task
 * through the GATT read callback. The struct copy is not atomic, so it sits
 * behind a spinlock.
 */
static portMUX_TYPE s_att_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_attitude_t s_attitude;
static imu_sample_t s_sample;
static bool s_attitude_valid;

/*
 * Attitude estimation state, only ever touched by the task calling
 * imu_read_attitude().
 *
 * Two bias mechanisms work together, and they compose additively:
 *   - s_gyr_z_bias comes from the short boot calibration, so the attitude is
 *     usable within a fraction of a second of power-on;
 *   - s_bias (FusionBias) tracks the residual on all three axes while the board
 *     sits still - which covers what a one-shot calibration cannot, namely
 *     thermal drift hours into a session.
 *
 * The second one is not optional here. The boot calibration measures a 0.32 s
 * window and leaves roughly 0.2 dps behind; yaw integrates that, which is
 * 12 degrees per minute. A 2026-09-23 comparison against a Mahony filter that
 * had no equivalent estimator measured exactly that drift, and is why this one
 * stays - see docs/imu.md sections 4.3 and 4.9.
 */
static float s_gyr_z_bias;
static int64_t s_last_us;
static FusionAhrs s_ahrs;
static FusionBias s_bias;

/* Private functions */
/*
 * Configure the accelerometer, gyroscope and the data path.
 *
 * Ranges and ODRs follow the values already validated on this hardware by the
 * ESP-Spot Lite self-test, so IMU_ACC_LSB_PER_G / IMU_GYR_LSB_PER_DPS in imu.h
 * stay correct as long as these two stay put.
 */
static int8_t imu_configure(void) {
    struct bmi2_sens_config cfg[2] = {0};

    cfg[BMI2_ACCEL].type = BMI2_ACCEL;
    cfg[BMI2_GYRO].type = BMI2_GYRO;

    int8_t rslt = bmi2_get_sensor_config(cfg, 2, s_handle);
    if (rslt != BMI2_OK) {
        return rslt;
    }

    cfg[BMI2_ACCEL].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
    cfg[BMI2_ACCEL].cfg.acc.range = BMI2_ACC_RANGE_4G;
    cfg[BMI2_ACCEL].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    cfg[BMI2_ACCEL].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    cfg[BMI2_GYRO].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
    cfg[BMI2_GYRO].cfg.gyr.range = BMI2_GYR_RANGE_500;
    cfg[BMI2_GYRO].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    cfg[BMI2_GYRO].cfg.gyr.noise_perf = BMI2_POWER_OPT_MODE;
    cfg[BMI2_GYRO].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    rslt = bmi2_set_sensor_config(cfg, 2, s_handle);
    if (rslt != BMI2_OK) {
        return rslt;
    }

    /* Advanced power save gates the sample path; leaving it on is the classic
     * "registers read back fine but the axes stay at zero" failure. */
    rslt = bmi2_set_adv_power_save(BMI2_DISABLE, s_handle);
    if (rslt != BMI2_OK) {
        return rslt;
    }

    uint8_t sens_list[2] = {BMI2_ACCEL, BMI2_GYRO};
    return bmi2_sensor_enable(sens_list, 2, s_handle);
}

static void IRAM_ATTR imu_drdy_isr(void *arg) {
    BaseType_t woken = pdFALSE;

    s_drdy_count++;

    if (s_drdy_waiter != NULL) {
        vTaskNotifyGiveFromISR(s_drdy_waiter, &woken);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/*
 * Measure the gyroscope Z zero-rate offset. Yaw is an integral, so whatever
 * offset is left in the rate accumulates linearly: the BMI270 datasheet puts
 * the zero-rate offset at around +/-1 dps, which without this step would be
 * 60 degrees of drift per minute.
 */
static void imu_calibrate_gyro_bias(void) {
    float sum = 0.0f;
    int taken = 0;

    for (int i = 0; i < IMU_GYRO_BIAS_SAMPLES; i++) {
        struct bmi2_sens_data data = {0};
        if (bmi2_get_sensor_data(&data, s_handle) == BMI2_OK) {
            sum += (float)data.gyr.z / IMU_GYR_LSB_PER_DPS;
            taken++;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_GYRO_BIAS_INTERVAL_MS));
    }

    s_gyr_z_bias = (taken > 0) ? (sum / (float)taken) : 0.0f;

    if (fabsf(s_gyr_z_bias) > IMU_GYRO_BIAS_WARN_DPS) {
        ESP_LOGW(TAG,
                 "imu: gyro Z bias %.2f dps over %d samples looks wrong; "
                 "was the board moving during boot? yaw will drift",
                 (double)s_gyr_z_bias, taken);
    } else {
        ESP_LOGI(TAG, "imu: gyro Z bias %.3f dps (%d samples)",
                 (double)s_gyr_z_bias, taken);
    }
}

/* Public functions */
esp_err_t imu_init(void) {
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CONFIG_IMU_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_IMU_I2C_SCL_GPIO,
        /* The board has no external pull-ups on these two lines, the internal
         * ones are the only thing holding the bus idle-high. */
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_IMU_I2C_FREQ_HZ,
    };

    s_i2c_bus = i2c_bus_create(IMU_I2C_PORT, &i2c_conf);
    if (s_i2c_bus == NULL) {
        ESP_LOGE(TAG, "imu: i2c bus create failed");
        return ESP_FAIL;
    }

    /*
     * bmi270_sensor_create() uploads the 8 KB Bosch configuration firmware,
     * which the BMI270 needs before it produces any sample at all.
     *
     * It has been observed to fail with ESP_ERR_INVALID_STATE on roughly one
     * power-on boot in four - the part does not always answer I2C yet by the
     * time this runs. Retrying is cheap insurance: without it the board comes
     * up with no IMU at all and stays that way until the next reset.
     */
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < IMU_INIT_ATTEMPTS; attempt++) {
        ret = bmi270_sensor_create(s_i2c_bus, &s_handle,
                                   bmi270_toy_config_file, 0);
        if (ret == ESP_OK && s_handle != NULL) {
            break;
        }
        ESP_LOGW(TAG, "imu: bmi270_sensor_create attempt %d/%d failed: %s",
                 attempt + 1, IMU_INIT_ATTEMPTS, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(IMU_INIT_RETRY_MS));
    }
    if (ret != ESP_OK || s_handle == NULL) {
        ESP_LOGE(TAG, "imu: bmi270_sensor_create failed after %d attempts: %s",
                 IMU_INIT_ATTEMPTS, esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    int8_t rslt = imu_configure();
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "imu: sensor configuration failed: %d", rslt);
        return ESP_FAIL;
    }

    /* Let the first samples settle before treating them as a bias reference. */
    vTaskDelay(pdMS_TO_TICKS(100));
    imu_calibrate_gyro_bias();

    /*
     * Fusion AHRS. Roll and pitch now come out of the filter instead of
     * straight from the accelerometer, so they stay correct while the board is
     * accelerating; yaw is still a relative angle (no magnetometer to observe
     * heading with) but is integrated from a continuously bias-corrected rate.
     *
     * The state is a quaternion throughout - Fusion integrates and normalises
     * the quaternion (FusionAhrs.c:167-169) and the Euler conversion happens
     * once, here, on the way out.
     */
    FusionAhrsInitialise(&s_ahrs);

    FusionAhrsSettings ahrs_settings = fusionAhrsDefaultSettings;
    ahrs_settings.sampleRate = IMU_ODR_HZ;
    ahrs_settings.convention = FusionConventionNwu;
    ahrs_settings.gain = IMU_AHRS_GAIN;
    /* Zero disables over-range detection; see the block comment at
     * IMU_GYRO_RANGE_DPS for why that is the deliberate choice here. */
    ahrs_settings.gyroscopeRange = 0.0f;
    ahrs_settings.accelerationRejection = IMU_ACC_REJECTION_DEG;
    ahrs_settings.rejectionTimeout = IMU_ACC_REJECTION_TIMEOUT_S;
    FusionAhrsSetSettings(&s_ahrs, &ahrs_settings);

    FusionBiasInitialise(&s_bias);

    FusionBiasSettings bias_settings = fusionBiasDefaultSettings;
    bias_settings.sampleRate = IMU_ODR_HZ;
    bias_settings.stationaryThreshold = IMU_BIAS_STATIONARY_DPS;
    bias_settings.stationaryPeriod = IMU_BIAS_STATIONARY_S;
    FusionBiasSetSettings(&s_bias, &bias_settings);

    /* The first imu_read_attitude() only establishes the time base and so
     * contributes no step to the filter. */
    s_last_us = 0;

    ESP_LOGI(TAG,
             "imu: BMI270 ready (SCL=%d SDA=%d, %d Hz), Fusion AHRS gain=%.2f",
             CONFIG_IMU_I2C_SCL_GPIO, CONFIG_IMU_I2C_SDA_GPIO,
             CONFIG_IMU_I2C_FREQ_HZ, (double)IMU_AHRS_GAIN);
    return ESP_OK;
}

esp_err_t imu_start_data_ready(TaskHandle_t waiter) {
    if (s_handle == NULL || waiter == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Data-ready onto INT1: push-pull, active high, not latched. Non-latched
     * gives one pulse per sample instead of a level that would have to be
     * cleared by reading a status register, which is the arrangement the
     * ESP-Spot Lite self-test verified on this hardware. */
    struct bmi2_int_pin_config pin = {0};
    pin.pin_type = BMI2_INT1;
    pin.int_latch = BMI2_INT_NON_LATCH;
    pin.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;
    pin.pin_cfg[0].lvl = BMI2_INT_ACTIVE_HIGH;
    pin.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
    pin.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;

    int8_t rslt = bmi2_set_int_pin_config(&pin, s_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "imu: INT1 pin configuration failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi2_map_data_int(BMI2_DRDY_INT, BMI2_INT1, s_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "imu: mapping data-ready to INT1 failed: %d", rslt);
        return ESP_FAIL;
    }

    /* Another component may have installed the ISR service already; that is
     * fine, it only has to happen once. */
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "imu: gpio_install_isr_service failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_IMU_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* Must agree with BMI2_INT_ACTIVE_HIGH above: the line rises on a new
         * sample, so the edge to catch is the rising one. */
        .intr_type = GPIO_INTR_POSEDGE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "imu: gpio_config on GPIO%d failed: %s",
                 CONFIG_IMU_INT_GPIO, esp_err_to_name(ret));
        return ret;
    }

    /* Publish the waiter before the handler can run. */
    s_drdy_waiter = waiter;

    ret = gpio_isr_handler_add(CONFIG_IMU_INT_GPIO, imu_drdy_isr, NULL);
    if (ret != ESP_OK) {
        s_drdy_waiter = NULL;
        ESP_LOGE(TAG, "imu: gpio_isr_handler_add failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "imu: sampling paced by data-ready, INT1 on GPIO%d",
             CONFIG_IMU_INT_GPIO);
    return ESP_OK;
}

esp_err_t imu_read(imu_sample_t *out) {
    if (s_handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    struct bmi2_sens_data data = {0};
    int8_t rslt = bmi2_get_sensor_data(&data, s_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "imu: bmi2_get_sensor_data failed: %d", rslt);
        return ESP_FAIL;
    }

    out->acc[0] = (float)data.acc.x / IMU_ACC_LSB_PER_G;
    out->acc[1] = (float)data.acc.y / IMU_ACC_LSB_PER_G;
    out->acc[2] = (float)data.acc.z / IMU_ACC_LSB_PER_G;
    out->gyr[0] = (float)data.gyr.x / IMU_GYR_LSB_PER_DPS;
    out->gyr[1] = (float)data.gyr.y / IMU_GYR_LSB_PER_DPS;
    out->gyr[2] = (float)data.gyr.z / IMU_GYR_LSB_PER_DPS;
    return ESP_OK;
}

esp_err_t imu_read_attitude(imu_attitude_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    imu_sample_t sample;
    esp_err_t ret = imu_read(&sample);
    if (ret != ESP_OK) {
        return ret;
    }

    int64_t now = esp_timer_get_time();
    float dt = 0.0f;
    if (s_last_us != 0) {
        dt = (float)(now - s_last_us) / 1000000.0f;
    }
    s_last_us = now;

    /*
     * Skip an implausible step rather than feeding it to the filter: dt is zero
     * on the very first call, and a long stall (debugger halt, blocked task)
     * would otherwise be integrated as one enormous rotation.
     */
    if (dt > 0.0f && dt < 0.5f) {
        /*
         * Plausibility gate. The accelerometer should read about 1 g and the
         * gyroscope should stay inside its configured range; anything else is a
         * corrupted reading rather than motion, and letting it through leaves a
         * permanent step in yaw.
         */
        const float acc_magnitude =
            sqrtf(sample.acc[0] * sample.acc[0] +
                  sample.acc[1] * sample.acc[1] +
                  sample.acc[2] * sample.acc[2]);
        const bool acc_ok = (acc_magnitude >= IMU_ACC_MIN_G) &&
                            (acc_magnitude <= IMU_ACC_MAX_G);
        const bool gyr_ok = (fabsf(sample.gyr[0]) <= IMU_GYR_MAX_DPS) &&
                            (fabsf(sample.gyr[1]) <= IMU_GYR_MAX_DPS) &&
                            (fabsf(sample.gyr[2]) <= IMU_GYR_MAX_DPS);

        if (!acc_ok || !gyr_ok) {
            s_discarded_samples++;
            /* Rate limited so a burst cannot bury the console, but the values
             * stay visible: without them the cause is a guess, and a glitch
             * looks exactly like real motion. */
            if (now - s_last_bad_log_us > IMU_BAD_SAMPLE_LOG_PERIOD_US) {
                s_last_bad_log_us = now;
                ESP_LOGW(TAG,
                         "imu: discarded implausible sample "
                         "(acc %.2f g, gyr %+.1f %+.1f %+.1f dps, %u total)",
                         (double)acc_magnitude, (double)sample.gyr[0],
                         (double)sample.gyr[1], (double)sample.gyr[2],
                         (unsigned)s_discarded_samples);
            }
            /* Deliberately falls through without updating the filter: the last
             * good attitude is republished, so the stream does not stall. */
        } else {
            /* Subtract the boot calibration first; FusionBias then estimates
             * what is left over, so the two corrections add up instead of
             * fighting. */
            FusionVector gyroscope = {
                .array = {sample.gyr[0], sample.gyr[1],
                          sample.gyr[2] - s_gyr_z_bias}};
            const FusionVector accelerometer = {
                .array = {sample.acc[0], sample.acc[1], sample.acc[2]}};

            gyroscope = FusionBiasUpdate(&s_bias, gyroscope);

            /* Feed the measured interval rather than the nominal one:
             * data-ready spacing jitters, and the filter integrates whatever it
             * is given. */
            FusionAhrsSetSamplePeriod(&s_ahrs, dt);
            FusionAhrsUpdateNoMagnetometer(&s_ahrs, gyroscope, accelerometer);
        }
    }

    const FusionEuler euler =
        FusionQuaternionToEuler(FusionAhrsGetQuaternion(&s_ahrs));

    const imu_attitude_t attitude = {
        .roll = euler.angle.roll,
        .pitch = euler.angle.pitch,
        .yaw = euler.angle.yaw,
    };

    portENTER_CRITICAL(&s_att_lock);
    s_attitude = attitude;
    /* Keep the sample that produced it: the dynamic test reads both together
     * to compare the filtered angles against the raw-accelerometer ones. */
    s_sample = sample;
    s_attitude_valid = true;
    portEXIT_CRITICAL(&s_att_lock);

    *out = attitude;
    return ESP_OK;
}

esp_err_t imu_get_latest_attitude(imu_attitude_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_att_lock);
    bool valid = s_attitude_valid;
    if (valid) {
        *out = s_attitude;
    }
    portEXIT_CRITICAL(&s_att_lock);

    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t imu_get_latest_sample(imu_sample_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_att_lock);
    bool valid = s_attitude_valid;
    if (valid) {
        *out = s_sample;
    }
    portEXIT_CRITICAL(&s_att_lock);

    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

uint32_t imu_get_data_ready_count(void) { return s_drdy_count; }

uint32_t imu_get_discarded_sample_count(void) { return s_discarded_samples; }
