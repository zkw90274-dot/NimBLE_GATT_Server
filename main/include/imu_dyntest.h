/*
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#ifndef IMU_DYNTEST_H
#define IMU_DYNTEST_H

#include <stdbool.h>

/*
 * Guided dynamic-accuracy test, driven entirely over the serial console.
 *
 * The task prints an instruction, samples while the operator performs it, then
 * prints that phase's statistics - so the resulting log is self-labelling and
 * needs no wall-clock correlation with what the operator did.
 *
 * Blocks for the whole sequence (about a minute), so give it its own task.
 * Enabled by CONFIG_IMU_DYNAMIC_TEST.
 */
void imu_dyntest_run(void);

/*
 * @brief Whether the guided test is currently running.
 *
 * While it is, the IMU task's own periodic log is suppressed: at ~150 ms per
 * line it buries the test's prompts and countdown completely, and the test
 * prints its own trace anyway.
 *
 * Safe to call when CONFIG_IMU_DYNAMIC_TEST is disabled - it simply returns
 * false because nothing ever starts the test.
 *
 * @return true while the test is running.
 */
bool imu_dyntest_is_running(void);

#endif // IMU_DYNTEST_H
