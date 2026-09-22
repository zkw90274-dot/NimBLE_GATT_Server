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

/* Private defines */
#define IMU_I2C_PORT I2C_NUM_0

/* Degrees per radian, same constant the ESP-Spot Lite self-test uses. */
#define RAD_TO_DEG (57.29578f)

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

/*
 * Latest attitude, written by the IMU task and read by the NimBLE host task
 * through the GATT read callback. The struct copy is not atomic, so it sits
 * behind a spinlock.
 */
static portMUX_TYPE s_att_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_attitude_t s_attitude;
static bool s_attitude_valid;

/* Yaw integration state, only ever touched by the task calling
 * imu_read_attitude(). */
static float s_yaw;
static float s_gyr_z_bias;
static int64_t s_last_us;

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

    /* bmi270_sensor_create() uploads the 8 KB Bosch configuration firmware,
     * which the BMI270 needs before it produces any sample at all. */
    esp_err_t ret = bmi270_sensor_create(s_i2c_bus, &s_handle,
                                         bmi270_toy_config_file, 0);
    if (ret != ESP_OK || s_handle == NULL) {
        ESP_LOGE(TAG, "imu: bmi270_sensor_create failed: %s",
                 esp_err_to_name(ret));
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

    /* Yaw starts at zero; the first imu_read_attitude() sets the time base and
     * therefore contributes no step. */
    s_yaw = 0.0f;
    s_last_us = 0;

    ESP_LOGI(TAG, "imu: BMI270 ready (SCL=%d SDA=%d, %d Hz)",
             CONFIG_IMU_I2C_SCL_GPIO, CONFIG_IMU_I2C_SDA_GPIO,
             CONFIG_IMU_I2C_FREQ_HZ);
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

    /* Roll and pitch from the gravity vector - formulas taken verbatim from
     * ESP-Spot Lite test_imu.c:test_imu_test_attitude(). */
    imu_attitude_t attitude;
    attitude.roll = atan2f(sample.acc[1], sample.acc[2]) * RAD_TO_DEG;
    attitude.pitch =
        atan2f(-sample.acc[0],
               sqrtf(sample.acc[1] * sample.acc[1] +
                     sample.acc[2] * sample.acc[2])) *
        RAD_TO_DEG;

    /* Yaw: integrate the bias-corrected gyroscope Z rate. */
    int64_t now = esp_timer_get_time();
    if (s_last_us != 0) {
        float dt = (float)(now - s_last_us) / 1000000.0f;
        /* Skip a bogus step rather than letting it inject a jump into the
         * integral - dt is 0 on the very first call, and a long stall (a
         * debugger halt, a blocked task) would otherwise add a huge slice. */
        if (dt > 0.0f && dt < 0.5f) {
            s_yaw += (sample.gyr[2] - s_gyr_z_bias) * dt;
            while (s_yaw > 180.0f) {
                s_yaw -= 360.0f;
            }
            while (s_yaw < -180.0f) {
                s_yaw += 360.0f;
            }
        }
    }
    s_last_us = now;
    attitude.yaw = s_yaw;

    portENTER_CRITICAL(&s_att_lock);
    s_attitude = attitude;
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

uint32_t imu_get_data_ready_count(void) { return s_drdy_count; }
