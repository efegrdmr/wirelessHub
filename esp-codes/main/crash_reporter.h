#pragma once
#include <stddef.h>

/**
 * Call at the very beginning of app_main (before hub_client_start).
 * - Checks reset reason; if it was a panic/WDT, schedules a crash report
 *   to be sent over UDP once the daemon is reachable.
 * - Re-initialises the RTC ring buffer so this boot's logs are captured
 *   for the NEXT potential crash.
 */
void crash_reporter_init(void);

/**
 * Called from log_forwarder's vprintf hook (no malloc, no FreeRTOS calls).
 * Appends the formatted log line to the RTC ring buffer.
 */
void crash_reporter_append(const char *msg, size_t len);
