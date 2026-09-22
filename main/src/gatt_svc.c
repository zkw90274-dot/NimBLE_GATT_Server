/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "gatt_svc.h"
#include "common.h"
#include "heart_rate.h"
#include "imu.h"
#include "led.h"

/* Private defines */
/* The IMU characteristic value is 3 little-endian IEEE-754 float32, in
 * degrees: roll, pitch, yaw. See imu_attitude_t in imu.h for what each one
 * means - in particular yaw is relative to boot, not an absolute heading. */
#define IMU_CHR_VALUE_LEN (12)

/* Private function declarations */
static int heart_rate_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static int led_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int imu_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

/* Private variables */
/* Heart rate service */
static const ble_uuid16_t heart_rate_svc_uuid = BLE_UUID16_INIT(0x180D);

static uint8_t heart_rate_chr_val[2] = {0};
static uint16_t heart_rate_chr_val_handle;
static const ble_uuid16_t heart_rate_chr_uuid = BLE_UUID16_INIT(0x2A37);

static uint16_t heart_rate_chr_conn_handle = 0;
static bool heart_rate_chr_conn_handle_inited = false;
static bool heart_rate_ind_status = false;

/* Automation IO service */
static const ble_uuid16_t auto_io_svc_uuid = BLE_UUID16_INIT(0x1815);
static uint16_t led_chr_val_handle;
static const ble_uuid128_t led_chr_uuid =
    BLE_UUID128_INIT(0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef,
                     0x12, 0x12, 0x25, 0x15, 0x00, 0x00);

/* IMU service */
/*
 * Custom 128-bit UUIDs local to this project - they are NOT assigned by the
 * Bluetooth SIG, so no standard 16-bit UUID is being squatted on:
 *   service        f0a1b2c3-d4e5-4f60-8a9b-000000000001
 *   characteristic f0a1b2c3-d4e5-4f60-8a9b-000000000002
 * BLE_UUID128_INIT() takes the bytes least significant first, i.e. the reverse
 * of how the UUID string above is written out.
 */
static const ble_uuid128_t imu_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9b, 0x8a, 0x60, 0x4f,
                     0xe5, 0xd4, 0xc3, 0xb2, 0xa1, 0xf0);
static const ble_uuid128_t imu_chr_uuid =
    BLE_UUID128_INIT(0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9b, 0x8a, 0x60, 0x4f,
                     0xe5, 0xd4, 0xc3, 0xb2, 0xa1, 0xf0);

static uint16_t imu_chr_val_handle;

static uint16_t imu_chr_conn_handle = 0;
static bool imu_chr_conn_handle_inited = false;
static bool imu_notify_status = false;

/* GATT services table */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    /* Heart rate service */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &heart_rate_svc_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){
             {/* Heart rate characteristic */
              .uuid = &heart_rate_chr_uuid.u,
              .access_cb = heart_rate_chr_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE,
              .val_handle = &heart_rate_chr_val_handle},
             {
                 0, /* No more characteristics in this service. */
             }}},

    /* Automation IO service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &auto_io_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){/* LED characteristic */
                                        {.uuid = &led_chr_uuid.u,
                                         .access_cb = led_chr_access,
                                         .flags = BLE_GATT_CHR_F_WRITE,
                                         .val_handle = &led_chr_val_handle},
                                        {0}},
    },

    /* IMU service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &imu_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){/* IMU sample characteristic */
                                        {.uuid = &imu_chr_uuid.u,
                                         .access_cb = imu_chr_access,
                                         .flags = BLE_GATT_CHR_F_READ |
                                                  BLE_GATT_CHR_F_NOTIFY,
                                         .val_handle = &imu_chr_val_handle},
                                        {0}},
    },

    {
        0, /* No more services. */
    },
};

/* Private functions */
static int heart_rate_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
    /* Local variables */
    int rc = 0;

    /* Handle access events */
    /* Note: Heart rate characteristic is read only */
    switch (ctxt->op) {

    /* Read characteristic event */
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* Verify connection handle */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "characteristic read; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG, "characteristic read by nimble stack; attr_handle=%d",
                     attr_handle);
        }

        /* Verify attribute handle */
        if (attr_handle == heart_rate_chr_val_handle) {
            /* Update access buffer value */
            heart_rate_chr_val[1] = get_heart_rate();
            rc = os_mbuf_append(ctxt->om, &heart_rate_chr_val,
                                sizeof(heart_rate_chr_val));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto error;

    /* Unknown event */
    default:
        goto error;
    }

error:
    ESP_LOGE(
        TAG,
        "unexpected access operation to heart rate characteristic, opcode: %d",
        ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

static int led_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    /* Local variables */
    int rc = 0;

    /* Handle access events */
    /* Note: LED characteristic is write only */
    switch (ctxt->op) {

    /* Write characteristic event */
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* Verify connection handle */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "characteristic write; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG,
                     "characteristic write by nimble stack; attr_handle=%d",
                     attr_handle);
        }

        /* Verify attribute handle */
        if (attr_handle == led_chr_val_handle) {
            /* Verify access buffer length */
            if (ctxt->om->om_len == 1) {
                /* Turn the LED on or off according to the operation bit */
                if (ctxt->om->om_data[0]) {
                    led_on();
                    ESP_LOGI(TAG, "led turned on!");
                } else {
                    led_off();
                    ESP_LOGI(TAG, "led turned off!");
                }
            } else {
                goto error;
            }
            return rc;
        }
        goto error;

    /* Unknown event */
    default:
        goto error;
    }

error:
    ESP_LOGE(TAG,
             "unexpected access operation to led characteristic, opcode: %d",
             ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

/* Append one float32, least significant byte first, and return the new cursor. */
static uint8_t *put_f32_le(uint8_t *out, float value) {
    uint32_t bits;

    /* Take the bit pattern, not a numeric conversion: the wire format is
     * IEEE-754, so the bytes have to be the float's own representation. */
    memcpy(&bits, &value, sizeof(bits));

    *out++ = (uint8_t)(bits & 0xFF);
    *out++ = (uint8_t)((bits >> 8) & 0xFF);
    *out++ = (uint8_t)((bits >> 16) & 0xFF);
    *out++ = (uint8_t)((bits >> 24) & 0xFF);
    return out;
}

/* Serialize an attitude as 3 little-endian float32: roll, pitch, yaw (deg). */
static void imu_attitude_to_le(const imu_attitude_t *attitude, uint8_t *out) {
    out = put_f32_le(out, attitude->roll);
    out = put_f32_le(out, attitude->pitch);
    out = put_f32_le(out, attitude->yaw);
}

static int imu_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    /* Local variables */
    int rc = 0;

    /* Handle access events */
    /* Note: IMU characteristic is read only; the notify path goes through the
     * same read handler, which is how NimBLE fetches the value to send. */
    switch (ctxt->op) {

    /* Read characteristic event */
    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* Verify connection handle */
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGD(TAG, "characteristic read; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGD(TAG,
                     "characteristic read by nimble stack; attr_handle=%d",
                     attr_handle);
        }

        /* Verify attribute handle */
        if (attr_handle == imu_chr_val_handle) {
            uint8_t val[IMU_CHR_VALUE_LEN];
            imu_attitude_t attitude;

            /* Serve the attitude the IMU task last computed, so that the value
             * sent in a notification is exactly the one that was logged. */
            if (imu_get_latest_attitude(&attitude) != ESP_OK) {
                ESP_LOGW(TAG,
                         "imu characteristic read before the first attitude");
                return BLE_ATT_ERR_UNLIKELY;
            }

            imu_attitude_to_le(&attitude, val);
            rc = os_mbuf_append(ctxt->om, val, sizeof(val));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto error;

    /* Unknown event */
    default:
        goto error;
    }

error:
    ESP_LOGE(TAG,
             "unexpected access operation to imu characteristic, opcode: %d",
             ctxt->op);
    return BLE_ATT_ERR_UNLIKELY;
}

/* Public functions */
void send_heart_rate_indication(void) {
    if (heart_rate_ind_status && heart_rate_chr_conn_handle_inited) {
        ble_gatts_indicate(heart_rate_chr_conn_handle,
                           heart_rate_chr_val_handle);
        ESP_LOGI(TAG, "heart rate indication sent!");
    }
}

void send_imu_notification(void) {
    /* Nothing to do until a peer subscribes: notifications are dropped
     * silently when no client has enabled the CCCD. */
    if (!imu_notify_status || !imu_chr_conn_handle_inited) {
        return;
    }

    int rc = ble_gatts_notify(imu_chr_conn_handle, imu_chr_val_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "imu notification failed; rc=%d", rc);
    }
}

/*
 *  Handle GATT attribute register events
 *      - Service register event
 *      - Characteristic register event
 *      - Descriptor register event
 */
void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    /* Local variables */
    char buf[BLE_UUID_STR_LEN];

    /* Handle GATT attributes register events */
    switch (ctxt->op) {

    /* Service register event */
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered service %s with handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                 ctxt->svc.handle);
        break;

    /* Characteristic register event */
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG,
                 "registering characteristic %s with "
                 "def_handle=%d val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;

    /* Descriptor register event */
    case BLE_GATT_REGISTER_OP_DSC:
        ESP_LOGD(TAG, "registering descriptor %s with handle=%d",
                 ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                 ctxt->dsc.handle);
        break;

    /* Unknown event */
    default:
        assert(0);
        break;
    }
}

/*
 *  GATT server subscribe event callback
 *      1. Update heart rate subscription status
 */

void gatt_svr_subscribe_cb(struct ble_gap_event *event) {
    /* Check connection handle */
    if (event->subscribe.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle);
    } else {
        ESP_LOGI(TAG, "subscribe by nimble stack; attr_handle=%d",
                 event->subscribe.attr_handle);
    }

    /* Check attribute handle */
    if (event->subscribe.attr_handle == heart_rate_chr_val_handle) {
        /* Update heart rate subscription status */
        heart_rate_chr_conn_handle = event->subscribe.conn_handle;
        heart_rate_chr_conn_handle_inited = true;
        heart_rate_ind_status = event->subscribe.cur_indicate;
    } else if (event->subscribe.attr_handle == imu_chr_val_handle) {
        /* Update IMU subscription status */
        imu_chr_conn_handle = event->subscribe.conn_handle;
        imu_chr_conn_handle_inited = true;
        imu_notify_status = event->subscribe.cur_notify;
        ESP_LOGI(TAG, "imu notifications %s",
                 imu_notify_status ? "enabled" : "disabled");
    }
}

/*
 *  GATT server initialization
 *      1. Initialize GATT service
 *      2. Update NimBLE host GATT services counter
 *      3. Add GATT services to server
 */
int gatt_svc_init(void) {
    /* Local variables */
    int rc = 0;

    /* 1. GATT service initialization */
    ble_svc_gatt_init();

    /* 2. Update GATT services counter */
    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    /* 3. Add GATT services */
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}
