/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "common.h"
#include "gap.h"
#include "gatt_svc.h"
#include "heart_rate.h"
#include "imu.h"
#include "led.h"
#include "esp_timer.h"

/* Library function declarations */
void ble_store_config_init(void);

/* Private function declarations */
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);
static void imu_task(void *param);

/* Private functions */
/*
 *  Stack event callback functions
 *      - on_stack_reset is called when host resets BLE stack due to errors
 *      - on_stack_sync is called when host has synced with controller
 */
static void on_stack_reset(int reason) {
    /* On reset, print reset reason to console */
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

static void on_stack_sync(void) {
    /* On stack sync, do advertising initialization */
    adv_init();
}

static void nimble_host_config_init(void) {
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Store host configuration */
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    /* Task entry log */
    ESP_LOGI(TAG, "nimble host task has been started!");

    /* This function won't return until nimble_port_stop() is executed */
    nimble_port_run();

    /* Clean up at exit */
    vTaskDelete(NULL);
}

static void heart_rate_task(void *param) {
    /* Task entry log */
    ESP_LOGI(TAG, "heart rate task has been started!");

    /* Loop forever */
    while (1) {
        /* Update heart rate value every 1 second */
        update_heart_rate();
        ESP_LOGI(TAG, "heart rate updated to %d", get_heart_rate());

        /* Send heart rate indication if enabled */
        send_heart_rate_indication();

        /* Sleep */
        vTaskDelay(HEART_RATE_TASK_PERIOD);
    }

    /* Clean up at exit */
    vTaskDelete(NULL);
}

static void imu_task(void *param) {
    int64_t last_notify_us = 0;

    /* Task entry log */
    ESP_LOGI(TAG, "imu task has been started!");

    /* Loop forever */
    while (1) {
        imu_attitude_t attitude;

        /* Wake on every data-ready edge from the BMI270. The timeout is a
         * fallback, not the normal path: without it a missing or misconfigured
         * interrupt line would stall sampling forever, silently. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_IMU_NOTIFY_PERIOD_MS));

        if (imu_read_attitude(&attitude) != ESP_OK) {
            continue;
        }

        /* The sensor free-runs at 200 Hz. Sampling on every data-ready keeps
         * the yaw integral tight, but BLE cannot carry 200 notifications a
         * second, so the radio is paced separately from the sampling. */
        int64_t now = esp_timer_get_time();
        if (last_notify_us != 0 &&
            (now - last_notify_us) <
                (int64_t)CONFIG_IMU_NOTIFY_PERIOD_MS * 1000) {
            continue;
        }
        last_notify_us = now;

        ESP_LOGI(TAG, "imu: roll %+7.2f  pitch %+7.2f  yaw %+7.2f  (deg)",
                 (double)attitude.roll, (double)attitude.pitch,
                 (double)attitude.yaw);
        send_imu_notification();
    }

    /* Clean up at exit */
    vTaskDelete(NULL);
}

void app_main(void) {
    /* Local variables */
    int rc = 0;
    esp_err_t ret;
    bool imu_ready = false;
    TaskHandle_t imu_task_handle = NULL;

    /* LED initialization */
    led_init();

    /* IMU initialization (BMI270 over I2C).
     * A failure here is logged but not fatal: the BLE server remains usable
     * and the IMU task is simply not started. */
    imu_ready = (imu_init() == ESP_OK);
    if (!imu_ready) {
        ESP_LOGE(TAG, "failed to initialize the IMU, IMU task not started");
    }

    /*
     * NVS flash initialization
     * Dependency of BLE stack to store configurations
     */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", ret);
        return;
    }

    /* NimBLE stack initialization */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

    /* GAP service initialization */
    rc = gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }

    /* GATT server initialization */
    rc = gatt_svc_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GATT server, error code: %d", rc);
        return;
    }

    /* NimBLE host configuration initialization */
    nimble_host_config_init();

    /* Start NimBLE host task thread and return */
    xTaskCreate(nimble_host_task, "NimBLE Host", 4*1024, NULL, 5, NULL);
    xTaskCreate(heart_rate_task, "Heart Rate", 4*1024, NULL, 5, NULL);
    if (imu_ready) {
        xTaskCreate(imu_task, "IMU", 4*1024, NULL, 5, &imu_task_handle);

        /* Arm the data-ready interrupt. Not fatal on failure: the IMU task
         * waits with a timeout, so it degrades to paced sampling. */
        if (imu_start_data_ready(imu_task_handle) != ESP_OK) {
            ESP_LOGW(TAG,
                     "failed to arm the IMU data-ready interrupt, "
                     "falling back to periodic sampling");
        } else {
            /* Health check on the interrupt wiring. A dead INT1 line looks
             * exactly like a working one from the outside - the sampling loop
             * runs on its timeout either way - so measure it once instead of
             * assuming. At the 200 Hz output data rate expect ~200 edges in
             * the first second. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "imu: %u data-ready interrupts in the first second",
                     (unsigned)imu_get_data_ready_count());
        }
    }
    return;
}
