/*
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Includes */
#include "imu_dyntest.h"
#include "common.h"
#include "imu.h"

#include <math.h>
#include "esp_timer.h"

/*
 * Guided dynamic-accuracy test.
 *
 * Why it is driven from the device rather than from the host: the firmware
 * knows exactly when each phase begins, so the log labels itself and nobody has
 * to correlate timestamps with what a human did by hand.
 *
 * Every phase records two sets of angles from the *same* sample:
 *
 *   - the AHRS output, and
 *   - what the plain accelerometer-only formulas would have said:
 *         roll  = atan2(acc_y, acc_z)
 *         pitch = atan2(-acc_x, sqrt(acc_y^2 + acc_z^2))
 *
 * Printing both is the whole point of the shake phase: the raw-acceleration
 * column swings with whatever linear acceleration the hand injects, while the
 * filtered column keeps tracking gravity - that gap is the reason a filter
 * exists here at all.
 *
 * Two of the eight phases are pure stillness (first and last). Their statistics
 * are the resting-wobble metric, which is how a change to the loop gain can be
 * judged without relying on a second run under different conditions.
 *
 * This harness was also what decided against the Mahony filter on 2026-09-23 -
 * it ran a third column for that comparison, on identical samples. See
 * docs/imu.md section 4.9.
 */

/* Private defines */
#define RAD_TO_DEG (57.29578f)

/* Live trace rate. Deliberately slow: one ESP_LOGI blocks its task for ~5 ms on
 * a 115200 baud console, and this test must not become the thing it measures. */
#define TRACE_PERIOD_MS 200

/* Number of trailing samples averaged to report the pose a phase ended in. */
#define TAIL_SAMPLES 5

/* Time to read the instruction before the countdown begins, and the spacing
 * between countdown steps. One second proved too quick: the prompt scrolled out
 * of view and there was no time to get a hand onto the board. */
#define READ_PAUSE_MS 2500
#define COUNTDOWN_STEP_MS 1500

/* Axis indices used by the statistics helpers. */
#define AXIS_ROLL 0
#define AXIS_PITCH 1
#define AXIS_YAW 2
#define AXES 3
/* The accelerometer-only reference cannot produce a yaw at all: with no
 * magnetometer there is nothing to derive a heading from. */
#define REF_AXES 2

/* Number of entries in s_phases. Only valid after the array is declared. */
#define PHASE_COUNT (sizeof(s_phases) / sizeof(s_phases[0]))

/* Private types */
typedef struct {
    const char *title;
    const char *hint;
    uint32_t seconds;
} phase_t;

/* Running statistics for one angle source over one phase. */
typedef struct {
    uint32_t n;
    float sum[AXES];
    float sum_sq[AXES];
    float min[AXES];
    float max[AXES];
    /*
     * Trailing window. A whole-phase mean is dominated by the movement itself
     * and says nothing about where the board ended up; the summary quotes the
     * average of this window instead.
     */
    float tail[TAIL_SAMPLES][AXES];
    uint32_t tail_count;
} angle_stats_t;

typedef struct {
    angle_stats_t fused;
    angle_stats_t accel;
} phase_stats_t;

/* Private variables */
/* Bilingual on purpose: the ASCII tag survives any console that mangles UTF-8,
 * the Chinese line is the friendly one. */
static const phase_t s_phases[] = {
    {"STILL BASELINE", "别碰板子，整段保持静止", 12},
    /*
     * "绕长边" was not enough to convey this. Two runs in a row the board was
     * raised to vertical instead, which measures pitch, not roll - and the
     * phase is named for roll. Say the axis, say the reference edge, and say
     * what the screen should show.
     */
    {"ROLL ONTO LONG EDGE ~90 deg",
     "只绕【长边】翻：板子长的那条边始终贴住桌面，短边那条翘起来，"
     "像合上一本书。翻到 90 度后停住别动。"
     "这是 roll 在变，看串口里 fused 行的第一个数", 8},
    {"LAY BACK FLAT", "放回平面，保持不动", 6},
    {"YAW ~90 deg",
     "板子全程平放贴桌面，像转盘子一样水平转约 90 度后停住。"
     "千万别把板子立起来 —— 立起来量到的就不是偏航角了", 10},
    {"YAW BACK", "板子仍平放，水平转回起始方向后保持不动", 8},
    {"SHAKE HARD", "大幅度快速晃动板子（注意别拽到 USB 线）", 8},
    {"SETTLE", "松手放回桌面，保持不动", 10},
    {"QUIET TAIL", "松手别碰，一直静止（20 秒，别急着动）", 20},
};

static phase_stats_t s_stats[PHASE_COUNT];

/* Set for the duration of the test so the IMU task stops logging over it. */
static volatile bool s_running;

/* Private functions */
/* Accelerometer-only attitude, i.e. what this firmware reported before any
 * filter was introduced. Kept for the side-by-side comparison. */
static void reference_angles(const imu_sample_t *sample, float *roll,
                             float *pitch) {
    *roll = atan2f(sample->acc[1], sample->acc[2]) * RAD_TO_DEG;
    *pitch = atan2f(-sample->acc[0],
                    sqrtf(sample->acc[1] * sample->acc[1] +
                          sample->acc[2] * sample->acc[2])) *
             RAD_TO_DEG;
}

static void angle_stats_reset(angle_stats_t *st) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < AXES; i++) {
        st->min[i] = 1e9f;
        st->max[i] = -1e9f;
    }
}

/*
 * @param angles  Source angles, AXES entries.
 * @param last    Highest index to accumulate. REF_AXES for the accelerometer-only
 *                source, which has no yaw.
 */
static void angle_stats_add(angle_stats_t *st, const float *angles,
                            const int last) {
    for (int i = 0; i < last; i++) {
        st->sum[i] += angles[i];
        st->sum_sq[i] += angles[i] * angles[i];
        if (angles[i] < st->min[i]) {
            st->min[i] = angles[i];
        }
        if (angles[i] > st->max[i]) {
            st->max[i] = angles[i];
        }
    }

    /* Slide the trailing window, newest first. */
    for (int k = TAIL_SAMPLES - 1; k > 0; k--) {
        for (int i = 0; i < AXES; i++) {
            st->tail[k][i] = st->tail[k - 1][i];
        }
    }
    for (int i = 0; i < AXES; i++) {
        st->tail[0][i] = angles[i];
    }
    if (st->tail_count < TAIL_SAMPLES) {
        st->tail_count++;
    }

    st->n++;
}

static float angle_stats_mean(const angle_stats_t *st, const int axis) {
    return (st->n > 0) ? (st->sum[axis] / (float)st->n) : 0.0f;
}

static float angle_stats_std(const angle_stats_t *st, const int axis) {
    if (st->n < 2) {
        return 0.0f;
    }
    const float mean = st->sum[axis] / (float)st->n;
    const float var = (st->sum_sq[axis] / (float)st->n) - (mean * mean);
    return (var > 0.0f) ? sqrtf(var) : 0.0f;
}

static float angle_stats_span(const angle_stats_t *st, const int axis) {
    if (st->n == 0) {
        return 0.0f;
    }
    return st->max[axis] - st->min[axis];
}

/* Average of the trailing window: the pose the phase ended in. */
static void phase_pose(const angle_stats_t *st, float out[AXES]) {
    const uint32_t n =
        (st->tail_count < TAIL_SAMPLES) ? st->tail_count : TAIL_SAMPLES;

    for (int i = 0; i < AXES; i++) {
        out[i] = 0.0f;
    }
    if (n == 0) {
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        for (int i = 0; i < AXES; i++) {
            out[i] += st->tail[k][i];
        }
    }
    for (int i = 0; i < AXES; i++) {
        out[i] /= (float)n;
    }
}

static void angle_stats_print(const char *label, const angle_stats_t *st,
                              const int axes) {
    static const char *names[AXES] = {"roll ", "pitch", "yaw  "};

    if (st->n == 0) {
        ESP_LOGW(TAG, "      %-18s (no samples)", label);
        return;
    }
    for (int i = 0; i < axes; i++) {
        ESP_LOGI(TAG,
                 "      %-18s %s mean %+8.2f  std %6.3f  min %+8.2f  max %+8.2f",
                 (i == 0) ? label : "", names[i], (double)angle_stats_mean(st, i),
                 (double)angle_stats_std(st, i), (double)st->min[i],
                 (double)st->max[i]);
    }
}

static void stats_print(const phase_stats_t *st) {
    ESP_LOGI(TAG, "      -------- Fused AHRS --------");
    angle_stats_print("fused", &st->fused, AXES);
    ESP_LOGI(TAG, "      ----- accel-only (no filter, reference) -----");
    angle_stats_print("accel-only", &st->accel, REF_AXES);
}

/*
 * Cue the operator. Without this the first second of every phase records them
 * still reaching for the board, which lands right in the statistics.
 *
 * The action line is repeated here on purpose: the phase header has scrolled
 * away by now, and this is the line the operator actually acts on.
 */
static void countdown(const phase_t *phase) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "      要做的动作：%s", phase->hint);
    vTaskDelay(pdMS_TO_TICKS(READ_PAUSE_MS));

    ESP_LOGI(TAG, "      ==========  3  ==========");
    vTaskDelay(pdMS_TO_TICKS(COUNTDOWN_STEP_MS));
    ESP_LOGI(TAG, "      ==========  2  ==========");
    vTaskDelay(pdMS_TO_TICKS(COUNTDOWN_STEP_MS));
    ESP_LOGI(TAG, "      ==========  1  ==========");
    vTaskDelay(pdMS_TO_TICKS(COUNTDOWN_STEP_MS));
    ESP_LOGI(TAG, "      >>>>>>>  开始!  <<<<<<<");
}

static void run_phase(const int index) {
    const phase_t *phase = &s_phases[index];
    phase_stats_t *st = &s_stats[index];

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "[%d/%d] %s  (%u s)", index + 1, (int)PHASE_COUNT,
             phase->title, (unsigned)phase->seconds);
    ESP_LOGI(TAG, "      %s", phase->hint);
    ESP_LOGI(TAG, "================================================");

    countdown(phase);

    angle_stats_reset(&st->fused);
    angle_stats_reset(&st->accel);

    const int64_t deadline =
        esp_timer_get_time() + (int64_t)phase->seconds * 1000000;
    while (esp_timer_get_time() < deadline) {
        imu_attitude_t attitude;
        imu_sample_t sample;

        if (imu_get_latest_attitude(&attitude) == ESP_OK &&
            imu_get_latest_sample(&sample) == ESP_OK) {
            const float fused[AXES] = {attitude.roll, attitude.pitch,
                                       attitude.yaw};
            float accel[AXES] = {0.0f, 0.0f, 0.0f};
            reference_angles(&sample, &accel[AXIS_ROLL], &accel[AXIS_PITCH]);

            angle_stats_add(&st->fused, fused, AXES);
            angle_stats_add(&st->accel, accel, REF_AXES);

            ESP_LOGI(TAG, "      fused %+7.2f %+7.2f %+7.2f  |  accel %+7.2f %+7.2f",
                     (double)fused[AXIS_ROLL], (double)fused[AXIS_PITCH],
                     (double)fused[AXIS_YAW], (double)accel[AXIS_ROLL],
                     (double)accel[AXIS_PITCH]);
        }
        vTaskDelay(pdMS_TO_TICKS(TRACE_PERIOD_MS));
    }

    stats_print(st);
}

/*
 * One comparison line: filtered against unfiltered, same statistic.
 *
 * A macro rather than a function taking the format string: IDF compiles with
 * -Werror=format=, which refuses a non-literal format outright.
 */
#define COMPARE_LINE(label, st, axis)                                         \
    ESP_LOGI(TAG, "%-18s %10.3f %10.3f", (label),                             \
             (double)angle_stats_std(&(st)->fused, (axis)),                   \
             (double)angle_stats_std(&(st)->accel, (axis)))

static void print_summary(void) {
    float base[AXES];
    float tilt[AXES];
    float flat[AXES];
    float turn[AXES];
    float back[AXES];
    float settle[AXES];
    float quiet[AXES];

    phase_pose(&s_stats[0].fused, base);
    phase_pose(&s_stats[1].fused, tilt);
    phase_pose(&s_stats[2].fused, flat);
    phase_pose(&s_stats[3].fused, turn);
    phase_pose(&s_stats[4].fused, back);
    phase_pose(&s_stats[6].fused, settle);
    phase_pose(&s_stats[7].fused, quiet);

    const phase_stats_t *sh = &s_stats[5];
    const phase_stats_t *qt = &s_stats[7];

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "################ SUMMARY ################");
    ESP_LOGI(TAG, "姿态角取自各阶段【结束时刻】，不是整段均值");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "baseline   roll %+7.2f  pitch %+7.2f  yaw %+7.2f",
             (double)base[AXIS_ROLL], (double)base[AXIS_PITCH],
             (double)base[AXIS_YAW]);
    ESP_LOGI(TAG, "roll~90    roll %+7.2f  pitch %+7.2f     (roll 期望接近 +/-90)",
             (double)tilt[AXIS_ROLL], (double)tilt[AXIS_PITCH]);
    ESP_LOGI(TAG, "flat       roll %+7.2f  pitch %+7.2f  yaw %+7.2f",
             (double)flat[AXIS_ROLL], (double)flat[AXIS_PITCH],
             (double)flat[AXIS_YAW]);
    ESP_LOGI(TAG, "  return   droll %+.2f  dpitch %+.2f  dyaw %+.2f   (应接近 0)",
             (double)(flat[AXIS_ROLL] - base[AXIS_ROLL]),
             (double)(flat[AXIS_PITCH] - base[AXIS_PITCH]),
             (double)(flat[AXIS_YAW] - base[AXIS_YAW]));
    ESP_LOGI(TAG, "turn       yaw %+7.2f   (变化 %+.2f，期望接近 +/-90)",
             (double)turn[AXIS_YAW],
             (double)(turn[AXIS_YAW] - base[AXIS_YAW]));
    ESP_LOGI(TAG, "back       yaw %+7.2f   (相对 baseline 误差 %+.2f，应接近 0)",
             (double)back[AXIS_YAW],
             (double)(back[AXIS_YAW] - base[AXIS_YAW]));
    ESP_LOGI(TAG, "settled    roll %+7.2f  pitch %+7.2f  yaw %+7.2f",
             (double)settle[AXIS_ROLL], (double)settle[AXIS_PITCH],
             (double)settle[AXIS_YAW]);
    ESP_LOGI(TAG, "quiet      roll %+7.2f  pitch %+7.2f  yaw %+7.2f   (结束姿态)",
             (double)quiet[AXIS_ROLL], (double)quiet[AXIS_PITCH],
             (double)quiet[AXIS_YAW]);

    /*
     * The headline table. Same samples, three algorithms.
     *
     * shake - how much the angle swings while the hand injects linear
     *         acceleration. This is where a filter earns its keep, and the
     *         accel-only column shows what is being filtered out.
     * quiet - pure stillness at the end, the resting-wobble metric. Lower is
     *         better, and this is the number a loop-gain change moves.
     */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=========== 滤波 vs 不滤波（同一批采样，std 越低越好）===========");
    ESP_LOGI(TAG, "                       %10s %10s", "fused", "accel-only");
    COMPARE_LINE("shake  roll std", sh, AXIS_ROLL);
    COMPARE_LINE("shake  pitch std", sh, AXIS_PITCH);
    COMPARE_LINE("quiet  roll std", qt, AXIS_ROLL);
    COMPARE_LINE("quiet  pitch std", qt, AXIS_PITCH);
    /* The accelerometer-only source has no yaw at all: with no magnetometer
     * there is nothing to derive a heading from, so that column is not
     * "missing", it does not exist. */
    ESP_LOGI(TAG, "%-18s %10.3f %10s", "quiet  yaw std",
             (double)angle_stats_std(&qt->fused, AXIS_YAW), "n/a");
    ESP_LOGI(TAG, "quiet span roll %6.3f  pitch %6.3f  yaw %6.3f  (极差)",
             (double)angle_stats_span(&qt->fused, AXIS_ROLL),
             (double)angle_stats_span(&qt->fused, AXIS_PITCH),
             (double)angle_stats_span(&qt->fused, AXIS_YAW));

    /* Corruption evidence. A growing number here is the sensor or the I2C link
     * misbehaving; those samples never reach either filter, so the angles above
     * stay clean regardless - but the count is what says whether it happened. */
    ESP_LOGI(TAG, "gate       %u rejected sample(s)",
             (unsigned)imu_get_discarded_sample_count());
    ESP_LOGI(TAG, "#########################################");
}

/* Public functions */
void imu_dyntest_run(void) {
    /* Silence the IMU task's periodic log for the duration. At ~150 ms per line
     * it drowns the prompts and the countdown completely, and this task prints
     * its own trace anyway. */
    s_running = true;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "############ IMU DYNAMIC ACCURACY TEST ############");
    ESP_LOGI(TAG, "全程约 155 秒。每个动作前会先给 2.5 秒读提示，再倒数 3-2-1，");
    ESP_LOGI(TAG, "看到「开始!」再做动作。动作之间请保持静止。");
    ESP_LOGI(TAG, "Follow each prompt after its countdown.");
    ESP_LOGI(TAG, "测试将在 10 秒后开始，请先静止...");
    vTaskDelay(pdMS_TO_TICKS(10000));

    for (int i = 0; i < (int)PHASE_COUNT; i++) {
        run_phase(i);
    }

    print_summary();

    s_running = false;

    /* Clean up at exit */
    vTaskDelete(NULL);
}

bool imu_dyntest_is_running(void) { return s_running; }
