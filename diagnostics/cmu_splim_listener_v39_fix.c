/*
 * cmu_splim_listener.c - Enhanced UDP Telemetry, Navigation & Real-Speed Overspeed Monitor for Mazda CMU
 * Targets: Mazda Connect CMU150 (FW 74.00.324 / Linux ARMv7)
 * Argon Audited & Enhanced: D-Bus bus_register + Multi-Type Speed unpack + UDP Over Fallback
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dbus/dbus.h>
#include "cmu_speed_normalization.h"

#define LISTEN_PORT            8888
#define DIAG_PORT              8889
#define SPLIM_OUTPUT_PATH      "/data_persist/splim"
#define SPLIM_TMP_PATH         "/data_persist/.splim.tmp"
#define NAV_OUTPUT_PATH        "/data_persist/nav"
#define NAV_TMP_PATH           "/data_persist/.nav.tmp"
#define OVER_FILE_PATH         "/data_persist/splim_over"
#define OVER_FILE_TMP          "/data_persist/.splim_over.tmp"
#define DBUS_SOCKET_PATH       "unix:path=/tmp/dbus_service_socket"

#define KEEPALIVE_INTERVAL_SEC 3
#define DATA_TIMEOUT_SEC       30

typedef struct {
    uint8_t speed_limit;
    uint8_t camera_speed;
    uint8_t is_overspeed;
    uint32_t real_car_speed;
    time_t  camera_expires;
    uint8_t last_written_limit;
    time_t  last_written_ts;
    time_t  last_rx_time;

    // Navigation state
    uint16_t nav_icon;
    uint32_t nav_distance;
    char     nav_road[128];
    time_t   nav_last_rx;
    time_t   nav_last_write_ts;
} cmu_telemetry_state_t;

static volatile sig_atomic_t g_running = 1;
static cmu_telemetry_state_t g_state;
static int g_last_over_state = -1;
static int g_udp_overspeed = 0;
static DBusConnection *g_dbus_conn = NULL;

/*
 * Evidence clock model.
 *
 * The CMU wall clock can boot at 1970, so time(NULL) alone is not a
 * trustworthy correlation key. The phone relay periodically sends:
 *   SYNC <phone_epoch_ms> <phone_monotonic_ms>
 * We anchor that wall-clock sample to the CMU monotonic clock and stamp every
 * listener/speed-debug record with sequence, CMU monotonic ms, calibrated
 * phone epoch ms, and UTC text. When the CMU clock is invalid or differs by
 * more than six hours, the root listener corrects CLOCK_REALTIME once; the
 * decision and result are saved in clock-sync-status.txt.
 */
static unsigned long long g_log_sequence = 0;
static bool g_phone_sync_valid = false;
static int64_t g_phone_sync_epoch_ms = 0;
static uint64_t g_phone_sync_cmu_mono_ms = 0;

static uint64_t cmu_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
    }
    return (uint64_t)time(NULL) * 1000ULL;
}

static int64_t cmu_log_epoch_ms(uint64_t mono_ms, const char **source) {
    time_t wall = time(NULL);
    if (g_phone_sync_valid && mono_ms >= g_phone_sync_cmu_mono_ms) {
        if (source) *source = "phone_sync";
        return g_phone_sync_epoch_ms + (int64_t)(mono_ms - g_phone_sync_cmu_mono_ms);
    }
    if (wall >= 1577836800) { /* 2020-01-01: reject the observed 1970 clock */
        if (source) *source = "cmu_clock";
        return (int64_t)wall * 1000LL;
    }
    if (source) *source = "unknown";
    return -1;
}

static void format_utc_ms(int64_t epoch_ms, char *out, size_t out_len) {
    if (epoch_ms < 0) {
        snprintf(out, out_len, "unknown");
        return;
    }
    time_t sec = (time_t)(epoch_ms / 1000LL);
    long millis = (long)(epoch_ms % 1000LL);
    struct tm tm_utc;
    if (!gmtime_r(&sec, &tm_utc)) {
        snprintf(out, out_len, "unknown");
        return;
    }
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, millis);
}

static void append_cmu_log_record(const char *path, const char *event) {
    if (!path || !event) return;
    char clean[768];
    snprintf(clean, sizeof(clean), "%s", event);
    size_t len = strlen(clean);
    while (len > 0 && (clean[len - 1] == '\r' || clean[len - 1] == '\n')) {
        clean[--len] = '\0';
    }

    uint64_t mono_ms = cmu_monotonic_ms();
    const char *source = NULL;
    int64_t epoch_ms = cmu_log_epoch_ms(mono_ms, &source);
    char utc[40];
    format_utc_ms(epoch_ms, utc, sizeof(utc));

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "[seq=%llu][cmu_mono_ms=%llu][epoch_ms=%lld][epoch_source=%s][utc=%s] %s\n",
                ++g_log_sequence,
                (unsigned long long)mono_ms,
                (long long)epoch_ms,
                source ? source : "unknown",
                utc,
                clean);
        fflush(f);
        fclose(f);
    }
}

static void write_clock_sync_status(int64_t target_epoch_ms, int64_t previous_epoch_ms,
                                     int result, int error_no, const char *action) {
    FILE *f = fopen("/data_persist/cp-hud-mod/clock-sync-status.txt", "w");
    if (!f) return;
    fprintf(f, "source=5se_gmaphud\n"
               "target_epoch_ms=%lld\n"
               "previous_epoch_ms=%lld\n"
               "result=%d\n"
               "errno=%d\n"
               "action=%s\n"
               "updated_epoch_ms=%lld\n",
            (long long)target_epoch_ms,
            (long long)previous_epoch_ms,
            result,
            error_no,
            action ? action : "unknown",
            (long long)((int64_t)time(NULL) * 1000LL));
    fflush(f);
    fclose(f);
}

static int64_t g_last_wall_sync_target_ms = 0;

static void sync_cmu_system_clock_if_needed(int64_t target_epoch_ms) {
    struct timespec current;
    if (clock_gettime(CLOCK_REALTIME, &current) != 0) {
        write_clock_sync_status(target_epoch_ms, -1, -1, errno, "clock_gettime_failed");
        return;
    }
    int64_t current_ms = (int64_t)current.tv_sec * 1000LL + current.tv_nsec / 1000000LL;
    int64_t delta_ms = target_epoch_ms - current_ms;
    bool invalid = current.tv_sec < 1577836800;
    bool large_delta = (delta_ms > 21600000LL || delta_ms < -21600000LL);
    if (!invalid && !large_delta) {
        write_clock_sync_status(target_epoch_ms, current_ms, 0, 0, "not_needed");
        return;
    }
    if (g_last_wall_sync_target_ms == target_epoch_ms) return;

    struct timespec target;
    target.tv_sec = (time_t)(target_epoch_ms / 1000LL);
    target.tv_nsec = (long)((target_epoch_ms % 1000LL) * 1000000LL);
    int result = clock_settime(CLOCK_REALTIME, &target);
    int error_no = (result == 0) ? 0 : errno;
    g_last_wall_sync_target_ms = target_epoch_ms;
    write_clock_sync_status(target_epoch_ms, current_ms, result, error_no,
                            result == 0 ? "set" : "set_failed");

    char event[256];
    snprintf(event, sizeof(event),
             "SYSTEM_CLOCK_SYNC target_epoch_ms=%lld previous_epoch_ms=%lld result=%d errno=%d",
             (long long)target_epoch_ms, (long long)current_ms, result, error_no);
    append_cmu_log_record("/tmp/cmu_listener.log", event);
}

static void apply_phone_time_sync(const char *buf) {
    long long phone_epoch_ms = 0;
    unsigned long long phone_mono_ms = 0;
    if (sscanf(buf, "SYNC %lld %llu", &phone_epoch_ms, &phone_mono_ms) >= 1 &&
        phone_epoch_ms >= 1577836800000LL && phone_epoch_ms <= 4102444800000LL) {
        g_phone_sync_epoch_ms = (int64_t)phone_epoch_ms;
        g_phone_sync_cmu_mono_ms = cmu_monotonic_ms();
        g_phone_sync_valid = true;
        sync_cmu_system_clock_if_needed(g_phone_sync_epoch_ms);
        (void)phone_mono_ms; /* retained in the raw SYNC event for audit */
    }
}

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static bool write_file_atomic(const char *final_path, const char *tmp_path, const char *content) {
    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;
    fputs(content, f);
    fflush(f);
    fclose(f);
    return rename(tmp_path, final_path) == 0;
}

static bool write_splim_atomic(const char *out_path, const char *tmp_path, uint8_t limit, time_t now) {
    char buf[64];
    if (limit > 0) {
        snprintf(buf, sizeof(buf), "%u %ld\n", (unsigned int)limit, (long)now);
    } else {
        buf[0] = '\0';
    }
    if (write_file_atomic(out_path, tmp_path, buf)) {
        g_state.last_written_limit = limit;
        g_state.last_written_ts = now;
        return true;
    }
    return false;
}

static bool write_nav_atomic(const char *out_path, const char *tmp_path,
                             uint16_t icon, uint32_t dist, const char *road, time_t ts) {
    char buf[256];
    if (icon > 0) {
        if (road && road[0] != '\0') {
            snprintf(buf, sizeof(buf), "%u %u %ld %s\n", (unsigned int)icon, (unsigned int)dist, (long)ts, road);
        } else {
            snprintf(buf, sizeof(buf), "%u %u %ld\n", (unsigned int)icon, (unsigned int)dist, (long)ts);
        }
    } else {
        buf[0] = '\0';
    }
    return write_file_atomic(out_path, tmp_path, buf);
}

static uint8_t get_effective_limit(time_t now) {
    if (g_state.last_rx_time == 0 || (now - g_state.last_rx_time >= DATA_TIMEOUT_SEC)) {
        return 0; // Clear display if disconnected
    }
    if (g_state.camera_speed > 0 && now < g_state.camera_expires) {
        return g_state.camera_speed;
    }
    return g_state.speed_limit;
}

// ── Persistent D-Bus BCM Connection for Real Car Speed ─────────────────────────
static bool init_bcm_dbus_connection(void) {
    if (g_dbus_conn && dbus_connection_get_is_connected(g_dbus_conn)) return true;

    if (g_dbus_conn) {
        dbus_connection_close(g_dbus_conn);
        dbus_connection_unref(g_dbus_conn);
        g_dbus_conn = NULL;
    }

    DBusError err;
    dbus_error_init(&err);
    // Use private connection so libdbus never caches or rejects re-registration on reconnect
    g_dbus_conn = dbus_connection_open_private(DBUS_SOCKET_PATH, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return false;
    }
    if (!g_dbus_conn) return false;

    // Register with message bus to enable method routing
    if (!dbus_bus_register(g_dbus_conn, &err)) {
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        dbus_connection_close(g_dbus_conn);
        dbus_connection_unref(g_dbus_conn);
        g_dbus_conn = NULL;
        return false;
    }

    return true;
}

static uint32_t query_cmu_vehicle_speed_raw(void) {
    if (!init_bcm_dbus_connection()) return 0;

    DBusMessage *msg = NULL;
    DBusMessage *reply = NULL;
    DBusError err;
    uint32_t raw_speed = 0;
    uint32_t arg_zero = 0;

    dbus_error_init(&err);

    msg = dbus_message_new_method_call(
        "com.jci.vbs.bcm",
        "/com/jci/vbs/bcm",
        "com.jci.vbs.bcm",
        "GetCarSpeed"
    );
    if (!msg) goto cleanup;

    if (!dbus_message_append_args(msg, DBUS_TYPE_UINT32, &arg_zero, DBUS_TYPE_INVALID)) {
        goto cleanup;
    }

    reply = dbus_connection_send_with_reply_and_block(g_dbus_conn, msg, 100, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        if (g_dbus_conn) {
            dbus_connection_close(g_dbus_conn);
            dbus_connection_unref(g_dbus_conn);
            g_dbus_conn = NULL;
        }
        goto cleanup;
    }
    if (!reply) goto cleanup;

    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)) {
        int arg_type = dbus_message_iter_get_arg_type(&iter);
        if (arg_type == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&iter, &raw_speed);
        } else if (arg_type == DBUS_TYPE_DOUBLE) {
            double val = 0.0;
            dbus_message_iter_get_basic(&iter, &val);
            if (val >= 0.0 && val <= 4294967295.0)
                raw_speed = (uint32_t)(val + 0.5);
        } else if (arg_type == DBUS_TYPE_INT32) {
            int32_t val = 0;
            dbus_message_iter_get_basic(&iter, &val);
            if (val > 0) raw_speed = (uint32_t)val;
        } else if (arg_type == DBUS_TYPE_UINT16) {
            uint16_t val = 0;
            dbus_message_iter_get_basic(&iter, &val);
            raw_speed = (uint32_t)val;
        }
    }

cleanup:
    if (msg) dbus_message_unref(msg);
    if (reply) dbus_message_unref(reply);
    
    // Preserve both raw and normalized values. FW 74.00.324 returns a
    // 10000 stationary baseline; treating that raw value as /100 created
    // a false 100 km/h overspeed while the vehicle was stopped.
    static time_t last_log = 0;
    time_t log_now = time(NULL);
    if (log_now != last_log) {
        last_log = log_now;
        char speed_event[192];
        snprintf(speed_event, sizeof(speed_event), "BCM raw=%u queried_speed=%d",
                 raw_speed, (int)normalize_cmu_speed_kmh(raw_speed));
        append_cmu_log_record("/tmp/cmu_speed_debug.log", speed_event);
    }

    return raw_speed;
}

static int32_t get_sanitized_vehicle_speed_kmh(void) {
    return normalize_cmu_speed_kmh(query_cmu_vehicle_speed_raw());
}

// Debounced update of overspeed flag into /data_persist/splim_over and /tmp/splim_over
static void update_overspeed_flag(int is_over) {
    int normalized = is_over ? 1 : 0;
    if (normalized != g_last_over_state) {
        char over_buf[16];
        snprintf(over_buf, sizeof(over_buf), "%d\n", normalized);
        write_file_atomic(OVER_FILE_PATH, OVER_FILE_TMP, over_buf);
        write_file_atomic("/tmp/splim_over", "/tmp/.splim_over.tmp", over_buf);
        g_last_over_state = normalized;
        g_state.is_overspeed = (uint8_t)normalized;
    }
}

static void update_hud_tick(time_t now, const char *out_path, const char *tmp_path,
                            const char *nav_out_path, const char *nav_tmp_path) {
    uint8_t eff = get_effective_limit(now);

    // Debounced write for Speed Limit
    if (eff != g_state.last_written_limit ||
        (now - g_state.last_written_ts >= KEEPALIVE_INTERVAL_SEC && eff > 0)) {
        write_splim_atomic(out_path, tmp_path, eff, now);
    }

    // ── Direct Speed Comparison vs VietMap Live Limit ──────────────────────────
    // Instead of relying on raw/noisy raw CMU CAN ticks which jitter,
    // we evaluate: if VietMap Live explicitly provides speed limit `eff`,
    // and real car speed `spd` is available, compare them directly.
    static time_t s_last_speed_poll = 0;
    static int s_clear_counter = 0;
    if (now != s_last_speed_poll) {
        s_last_speed_poll = now;
        if (eff >= 5 && eff <= 140) {
            int32_t spd = get_sanitized_vehicle_speed_kmh();
            g_state.real_car_speed = (spd >= 0) ? (uint32_t)spd : 0;
            int is_over = 0;

            if (spd < 0) {
                // If CAN speed is unreadable, default to NO blinking (safe fallback)
                is_over = 0;
            } else {
                // Strict Comparative Logic:
                // Trigger overspeed ONLY when real speed exceeds VietMap speed limit + 2 km/h margin.
                // Clear overspeed ONLY when real speed drops back to or below VietMap speed limit for 2 consecutive ticks.
                if (g_last_over_state <= 0) {
                    if (spd >= (int32_t)(eff + 2)) {
                        is_over = 1;
                        s_clear_counter = 0;
                    } else {
                        is_over = 0;
                    }
                } else {
                    if (spd <= (int32_t)eff) {
                        s_clear_counter++;
                        if (s_clear_counter >= 2) {
                            is_over = 0;
                        } else {
                            is_over = 1; // Hold blink state during transient dips
                        }
                    } else {
                        s_clear_counter = 0;
                        is_over = 1;
                    }
                }
            }
            update_overspeed_flag(is_over);
        } else {
            s_clear_counter = 0;
            update_overspeed_flag(0);
        }
    }

    // Debounced write / timeout for Navigation
    if (g_state.nav_last_rx > 0 && (now - g_state.nav_last_rx < DATA_TIMEOUT_SEC)) {
        if (now - g_state.nav_last_write_ts >= KEEPALIVE_INTERVAL_SEC) {
            write_nav_atomic(nav_out_path, nav_tmp_path,
                             g_state.nav_icon, g_state.nav_distance, g_state.nav_road, now);
            g_state.nav_last_write_ts = now;
        }
    } else if (g_state.nav_last_rx > 0 && (now - g_state.nav_last_rx >= DATA_TIMEOUT_SEC)) {
        // Clear navigation display
        write_nav_atomic(nav_out_path, nav_tmp_path, 0, 0, "", now);
        g_state.nav_last_rx = 0;
    }
}

// Explicit VietMap Live Maneuver ID -> Mazda HUD Icon Mapping
static uint16_t map_vietmap_to_mazda_icon(int vm_icon) {
    if (vm_icon <= 0) return 0;
    switch (vm_icon) {
        case 1:  return 1;  // HUD_STRAIGHT
        case 2:  return 2;  // HUD_LEFT
        case 3:  return 3;  // HUD_RIGHT
        case 4:  return 4;  // HUD_SLIGHT_LEFT
        case 5:  return 5;  // HUD_SLIGHT_RIGHT
        case 6:  return 11; // HUD_SHARP_LEFT (Mazda: 11)
        case 7:  return 9;  // HUD_SHARP_RIGHT (Mazda: 9)
        case 8:  return 13; // HUD_U_TURN_LEFT (Mazda: 13)
        case 9:  return 16; // HUD_MERGE_LEFT (Mazda: 16)
        case 10: return 10; // HUD_U_TURN_RIGHT (Mazda: 10)
        case 11: return 8;  // HUD_DESTINATION (Mazda: 8)
        default: return (vm_icon >= 1 && vm_icon <= 60) ? (uint16_t)vm_icon : 0;
    }
}

static void parse_telemetry_packet(const char *buf, size_t len, time_t now,
                                   const char *out_path, const char *tmp_path,
                                   const char *nav_out_path, const char *nav_tmp_path) {
    if (!buf || len == 0) return;

    if (strncmp(buf, "SYNC ", 5) == 0) {
        apply_phone_time_sync(buf);
        int64_t synced_epoch_ms = cmu_log_epoch_ms(cmu_monotonic_ms(), NULL);
        now = (synced_epoch_ms >= 0) ? (time_t)(synced_epoch_ms / 1000LL) : time(NULL);
    }

    static char last_logged_buf[256] = "";
    static uint64_t last_log_mono_ms = 0;
    uint64_t current_log_mono_ms = cmu_monotonic_ms();
    if (strcmp(buf, last_logged_buf) != 0 ||
        (last_log_mono_ms == 0 || current_log_mono_ms - last_log_mono_ms >= 3000ULL)) {
        char rx_event[768];
        snprintf(rx_event, sizeof(rx_event), "RX: %s", buf);
        append_cmu_log_record("/tmp/cmu_listener.log", rx_event);
        strncpy(last_logged_buf, buf, sizeof(last_logged_buf) - 1);
        last_logged_buf[sizeof(last_logged_buf) - 1] = '\0';
        last_log_mono_ms = current_log_mono_ms;
    }

    if (strncmp(buf, "OVER", 4) == 0) {
        int over = 0;
        if (sscanf(buf, "OVER %d", &over) >= 1) {
            g_udp_overspeed = (over != 0) ? 1 : 0;
            g_state.last_rx_time = now;
            update_hud_tick(now, out_path, tmp_path, nav_out_path, nav_tmp_path);
        }
    } else if (strncmp(buf, "LIMIT", 5) == 0) {
        int limit = 0;
        int cam_spd = 0;
        int is_over = 0;
        int matched = sscanf(buf, "LIMIT %d %d %d", &limit, &cam_spd, &is_over);
        if (matched >= 1) {
            if ((limit >= 5 && limit <= 140) || limit == 0) {
                g_state.speed_limit = (uint8_t)limit;
                g_state.last_rx_time = now;
            }
            if (matched >= 2 && cam_spd >= 20 && cam_spd <= 140) {
                g_state.camera_speed = (uint8_t)cam_spd;
                g_state.camera_expires = now + 15;
                g_state.last_rx_time = now;
            } else {
                g_state.camera_speed = 0;
                g_state.camera_expires = 0;
            }
            if (matched >= 3) {
                g_udp_overspeed = (is_over != 0) ? 1 : 0;
            } else {
                g_udp_overspeed = 0; // If VietMap/tweak does not explicitly send overspeed flag, default to 0 (no blinking)
            }
            update_hud_tick(now, out_path, tmp_path, nav_out_path, nav_tmp_path);
        }
    } else if (strncmp(buf, "NAV", 3) == 0) {
        int icon = 0;
        unsigned int dist = 0;
        char road[128] = {0};
        int matched = sscanf(buf, "NAV %d %u %127[^\r\n]", &icon, &dist, road);
        if (matched >= 2) {
            uint16_t mapped_icon = 0;
            if (icon >= 1 && icon <= 60) {
                mapped_icon = (uint16_t)icon;
            } else if (icon > 0) {
                mapped_icon = map_vietmap_to_mazda_icon(icon);
            }

            // Drop repeated idle packets; clear exactly once on active -> inactive.
            if (g_state.nav_icon == 0 && mapped_icon == 0) {
                return;
            }
            if (g_state.nav_icon > 0 && mapped_icon == 0) {
                g_state.nav_icon = 0;
                g_state.nav_distance = 0;
                g_state.nav_road[0] = '\0';
                write_nav_atomic(nav_out_path, nav_tmp_path, 0, 0, "", now);
                g_state.nav_last_rx = 0;
                g_state.nav_last_write_ts = 0;
                return;
            }

            bool changed = (g_state.nav_icon != mapped_icon) ||
                           (g_state.nav_distance != dist) ||
                           (strncmp(g_state.nav_road, road, sizeof(g_state.nav_road)) != 0);
            g_state.nav_last_rx = now;
            if (!changed) return;

            g_state.nav_icon = mapped_icon;
            g_state.nav_distance = dist;
            snprintf(g_state.nav_road, sizeof(g_state.nav_road), "%s", road);
            write_nav_atomic(nav_out_path, nav_tmp_path,
                             g_state.nav_icon, g_state.nav_distance, g_state.nav_road, now);
            g_state.nav_last_write_ts = now;
        }
    }
}

// Mini HTTP/Diag server helper
static void dump_file_to_socket(int fd, const char *label, const char *path) {
    char sec_buf[256];
    snprintf(sec_buf, sizeof(sec_buf), "--- %s (%s) ---\n", label, path);
    send(fd, sec_buf, strlen(sec_buf), 0);
    FILE *f = fopen(path, "r");
    if (!f) {
        send(fd, "(file not found)\n\n", 18, 0);
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        send(fd, line, strlen(line), 0);
    }
    fclose(f);
    send(fd, "\n", 1, 0);
}

static void handle_diag_http_client(int client_fd, const char *splim_path, const char *nav_path) {
    /* Read the request line before replying so /diag/all can expose the
     * bounded CMU evidence bundle to the 5SE collector. The socket timeout
     * prevents a half-open HTTP client from blocking the listener loop. */
    struct timeval rtv;
    rtv.tv_sec = 1;
    rtv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    char request[512] = {0};
    ssize_t request_len = recv(client_fd, request, sizeof(request) - 1, 0);
    bool all_logs = (request_len > 0 && strstr(request, "GET /diag/all") != NULL);

    char header[] = 
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Connection: close\r\n\r\n";
    send(client_fd, header, strlen(header), 0);

    time_t now = time(NULL);
    char status_buf[1024];
    int splim_age = (g_state.last_rx_time > 0) ? (int)(now - g_state.last_rx_time) : -1;
    int nav_age = (g_state.nav_last_rx > 0) ? (int)(now - g_state.nav_last_rx) : -1;
    const char *clock_source = NULL;
    int64_t clock_epoch_ms = cmu_log_epoch_ms(cmu_monotonic_ms(), &clock_source);

    snprintf(status_buf, sizeof(status_buf),
        "=== MAZDA CMU TELEMETRY LISTENER STATUS ===\n"
        "Time: %ld\n"
        "Evidence epoch_ms: %lld (source=%s, phone_sync=%s)\n"
        "Speed Limit: %u km/h (cam: %u, last rx: %d sec ago)\n"
        "Real Vehicle Speed: %u km/h (Overspeed: %s)\n"
        "Nav Maneuver: icon=%u, dist=%u m, road=\"%s\" (last rx: %d sec ago)\n"
        "\n",
        (long)now,
        (long long)clock_epoch_ms,
        clock_source ? clock_source : "unknown",
        g_phone_sync_valid ? "yes" : "no",
        g_state.speed_limit, g_state.camera_speed, splim_age,
        g_state.real_car_speed, g_state.is_overspeed ? "YES (BLINKING)" : "NO",
        g_state.nav_icon, g_state.nav_distance, g_state.nav_road, nav_age
    );
    send(client_fd, status_buf, strlen(status_buf), 0);

    dump_file_to_socket(client_fd, "Active Speed Limit File", splim_path);
    dump_file_to_socket(client_fd, "Active Navigation File", nav_path);
    dump_file_to_socket(client_fd, "Active Overspeed File", OVER_FILE_PATH);
    dump_file_to_socket(client_fd, "RAM Overspeed File", "/tmp/splim_over");
    dump_file_to_socket(client_fd, "CMU Listener Log", "/tmp/cmu_listener.log");

    if (all_logs) {
        static const char *const evidence_paths[] = {
            "/tmp/carplay_bridge.log",
            "/tmp/cmu_speed_debug.log",
            "/tmp/live_hud_drive.log",
            "/tmp/live_speed_raw.log",
            "/tmp/mnt/sda1/logs/live_hud_drive.log",
            "/tmp/mnt/sda1/logs/live_speed_raw.log",
            "/mnt/sda1/logs/live_hud_drive.log",
            "/mnt/sda1/logs/live_speed_raw.log",
            "/data_persist/cp-hud-mod/build-info.txt",
            "/data_persist/cp-hud-mod/manifest.txt",
            "/data_persist/cp-hud-mod/clock-sync-status.txt",
            "/data_persist/gmaphud-session.txt"
        };
        const size_t evidence_count = sizeof(evidence_paths) / sizeof(evidence_paths[0]);
        for (size_t i = 0; i < evidence_count; ++i) {
            char label[128];
            snprintf(label, sizeof(label), "Evidence %zu", i + 1);
            dump_file_to_socket(client_fd, label, evidence_paths[i]);
        }
    }
}

int main(int argc, char **argv) {
    const char *out_path = SPLIM_OUTPUT_PATH;
    const char *tmp_path = SPLIM_TMP_PATH;
    const char *nav_out_path = NAV_OUTPUT_PATH;
    const char *nav_tmp_path = NAV_TMP_PATH;
    int port = LISTEN_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--tmp") == 0 && i + 1 < argc) {
            tmp_path = argv[++i];
        } else if (strcmp(argv[i], "--nav-out") == 0 && i + 1 < argc) {
            nav_out_path = argv[++i];
        } else if (strcmp(argv[i], "--nav-tmp") == 0 && i + 1 < argc) {
            nav_tmp_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int udp_sock = socket(AF_INET6, SOCK_DGRAM, 0);
    int is_v6 = 0;
    int opt = 1;
    int bind_ok = -1;

    if (udp_sock >= 0) {
        int v6only = 0; // Dual-stack: receive both IPv6 (cmu.local) and IPv4-mapped (::ffff:192.168.53.1)
        if (setsockopt(udp_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) == 0) {
            setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            struct sockaddr_in6 addr6;
            memset(&addr6, 0, sizeof(addr6));
            addr6.sin6_family = AF_INET6;
            addr6.sin6_addr = in6addr_any;
            addr6.sin6_port = htons((uint16_t)port);

            bind_ok = bind(udp_sock, (struct sockaddr *)&addr6, sizeof(addr6));
            if (bind_ok == 0) {
                is_v6 = 1;
            }
        }

        // Fallback: If IPV6_V6ONLY=0 or IPv6 bind() failed, close v6 socket to fall back cleanly to IPv4
        if (bind_ok < 0) {
            close(udp_sock);
            udp_sock = -1;
        }
    }

    // Pure IPv4 Fallback path (if AF_INET6 unsupported, disabled, or failed to bind dual-stack)
    if (udp_sock < 0) {
        udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock < 0) {
            return 1;
        }
        setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = htonl(INADDR_ANY);
        addr4.sin_port = htons((uint16_t)port);

        bind_ok = bind(udp_sock, (struct sockaddr *)&addr4, sizeof(addr4));
        if (bind_ok < 0) {
            close(udp_sock);
            return 1; // Mandatory fail-fast: never enter recvfrom() on an unbound UDP socket
        }
        is_v6 = 0;
    }

    int flags = fcntl(udp_sock, F_GETFL, 0);
    fcntl(udp_sock, F_SETFL, flags | O_NONBLOCK);
    (void)is_v6;

    int diag_sock = socket(AF_INET6, SOCK_STREAM, 0);
    int diag_bind_ok = -1;
    if (diag_sock >= 0) {
        int v6only = 0;
        if (setsockopt(diag_sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) == 0) {
            setsockopt(diag_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in6 diag_addr6;
            memset(&diag_addr6, 0, sizeof(diag_addr6));
            diag_addr6.sin6_family = AF_INET6;
            diag_addr6.sin6_addr = in6addr_any;
            diag_addr6.sin6_port = htons(DIAG_PORT);
            diag_bind_ok = bind(diag_sock, (struct sockaddr *)&diag_addr6, sizeof(diag_addr6));
        }
        if (diag_bind_ok < 0) {
            close(diag_sock);
            diag_sock = -1;
        }
    }

    if (diag_sock < 0) {
        diag_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (diag_sock >= 0) {
            setsockopt(diag_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in diag_addr4;
            memset(&diag_addr4, 0, sizeof(diag_addr4));
            diag_addr4.sin_family = AF_INET;
            diag_addr4.sin_addr.s_addr = htonl(INADDR_ANY);
            diag_addr4.sin_port = htons(DIAG_PORT);
            diag_bind_ok = bind(diag_sock, (struct sockaddr *)&diag_addr4, sizeof(diag_addr4));
            if (diag_bind_ok < 0) {
                close(diag_sock);
                diag_sock = -1;
            }
        }
    }

    if (diag_sock >= 0) {
        listen(diag_sock, 5);
        int dflags = fcntl(diag_sock, F_GETFL, 0);
        fcntl(diag_sock, F_SETFL, dflags | O_NONBLOCK);
    }

    memset(&g_state, 0, sizeof(g_state));

    char init_event[192];
    snprintf(init_event, sizeof(init_event),
             "cmu_splim_listener started with CAN Speed Overspeed Monitor (UDP :%d, HTTP Diag :%d)",
             port, DIAG_PORT);
    append_cmu_log_record("/tmp/cmu_listener.log", init_event);

    char buf[512];
    while (g_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(udp_sock, &fds);
        int max_fd = udp_sock;
        if (diag_sock >= 0) {
            FD_SET(diag_sock, &fds);
            if (diag_sock > max_fd) max_fd = diag_sock;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms tick for responsive updates

        int res = select(max_fd + 1, &fds, NULL, NULL, &tv);
        time_t now = time(NULL);

        if (res > 0) {
            if (FD_ISSET(udp_sock, &fds)) {
                struct sockaddr_storage src;
                socklen_t srclen = sizeof(src);
                ssize_t n = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &srclen);
                if (n > 0) {
                    buf[n] = '\0';
                    parse_telemetry_packet(buf, (size_t)n, now, out_path, tmp_path, nav_out_path, nav_tmp_path);
                }
            }
            if (diag_sock >= 0 && FD_ISSET(diag_sock, &fds)) {
                struct sockaddr_storage client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(diag_sock, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd >= 0) {
                    handle_diag_http_client(client_fd, out_path, nav_out_path);
                    close(client_fd);
                }
            }
        }

        update_hud_tick(now, out_path, tmp_path, nav_out_path, nav_tmp_path);
    }

    close(udp_sock);
    if (diag_sock >= 0) close(diag_sock);
    write_splim_atomic(out_path, tmp_path, 0, time(NULL));
    update_overspeed_flag(0);
    write_nav_atomic(nav_out_path, nav_tmp_path, 0, 0, "", time(NULL));
    if (g_dbus_conn) {
        dbus_connection_unref(g_dbus_conn);
        g_dbus_conn = NULL;
    }
    return 0;
}
