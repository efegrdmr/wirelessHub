#pragma once

/*
 * Log.h — lightweight logging for the WirelessHub daemon.
 *
 * Four levels (all enabled by default; compile with -DLOG_DEBUG=0 to suppress DBG):
 *   LOG_ERR  — fatal/hard errors written to stderr (red)
 *   LOG_WARN — recoverable anomalies written to stderr (yellow)
 *   LOG_INFO — important lifecycle events written to stdout (green)
 *   LOG_DBG  — verbose per-packet / data-flow trace written to stdout (cyan)
 *
 * Each line is prefixed with HH:MM:SS.mmm for easy correlation with ESP logs.
 */

#include <cstdio>
#include <ctime>

#ifndef LOG_DEBUG
#  define LOG_DEBUG 1
#endif

namespace wh_log {
    // Returns a pointer to a static buffer — single-threaded daemon only.
    inline const char* ts()
    {
        static char buf[16];
        struct timespec tp{};
        clock_gettime(CLOCK_REALTIME, &tp);
        struct tm t{};
        localtime_r(&tp.tv_sec, &t);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                 t.tm_hour, t.tm_min, t.tm_sec,
                 static_cast<int>(tp.tv_nsec / 1'000'000L));
        return buf;
    }
} // namespace wh_log

// ── Macros ────────────────────────────────────────────────────────────────────

#define LOG_ERR(fmt, ...) \
    fprintf(stderr, "%s \033[31m[ERR] \033[0m" fmt "\n", wh_log::ts(), ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    fprintf(stderr, "%s \033[33m[WARN]\033[0m " fmt "\n", wh_log::ts(), ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    fprintf(stdout, "%s \033[32m[INFO]\033[0m " fmt "\n", wh_log::ts(), ##__VA_ARGS__)

#if LOG_DEBUG
#  define LOG_DBG(fmt, ...) \
       fprintf(stdout, "%s \033[36m[DBG] \033[0m" fmt "\n", wh_log::ts(), ##__VA_ARGS__)
#else
#  define LOG_DBG(fmt, ...) do {} while (0)
#endif
