#include "app_log.h"
#include "esp_partition.h"
#include "esp_log.h"   // esp_log_timestamp() only — see app_log.h banner
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *TAG = "app_log";

// ---- Flash ring buffer -------------------------------------------------
// Raw (no filesystem) partition, divided into fixed-size sector-aligned
// slots. Self-describing: on boot we scan every slot once to find the
// highest sequence number and resume there, rather than tracking a
// separate write cursor (which would itself need persisting somewhere).
// A sector is erased exactly once, right before its first record of a
// new lap is written — the rest of that sector's records simply overwrite
// into an already-erased region.

#define RECORD_MAGIC        0x464C4F47u   // 'FLOG'
#define SECTOR_SIZE         4096u
#define RECORD_SIZE         128u
#define RECORDS_PER_SECTOR  (SECTOR_SIZE / RECORD_SIZE)   // 32

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t timestamp_ms;
    uint8_t  level;          // app_log_level_t, always WARN or ERROR
    char     tag[16];
    char     msg[99];        // pads the struct to RECORD_SIZE (128) exactly:
                              // 4+4+4+1+16+99 = 128
} flash_record_t;

_Static_assert(sizeof(flash_record_t) == RECORD_SIZE, "flash_record_t must be exactly RECORD_SIZE");

static const esp_partition_t *s_part      = NULL;
static uint32_t                s_total_slots = 0;
static uint32_t                s_write_slot  = 0;
static uint32_t                s_next_seq    = 0;
static SemaphoreHandle_t       s_lock        = NULL;
static bool                    s_debug_on    = false;

void app_log_set_debug_enabled(bool enabled) { s_debug_on = enabled; }
bool app_log_debug_enabled(void)             { return s_debug_on; }

void app_log_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       (esp_partition_subtype_t)0x40 /* see partitions.csv */,
                                       "fault_log");
    if (!s_part) {
        ESP_LOGE(TAG, "fault_log partition not found — flash fault capture disabled");
        return;
    }
    s_total_slots = s_part->size / RECORD_SIZE;

    // Scan every slot once at boot to find where to resume. Cheap (up to
    // ~1000 short raw-flash reads) and needs no separate persisted cursor.
    uint32_t highest_seq  = 0;
    uint32_t highest_slot = 0;
    bool     found_any    = false;
    flash_record_t rec;
    for (uint32_t slot = 0; slot < s_total_slots; slot++) {
        if (esp_partition_read(s_part, slot * RECORD_SIZE, &rec, RECORD_SIZE) != ESP_OK)
            continue;
        if (rec.magic != RECORD_MAGIC) continue;
        if (!found_any || rec.seq > highest_seq) {
            highest_seq  = rec.seq;
            highest_slot = slot;
            found_any    = true;
        }
    }

    if (found_any) {
        s_next_seq   = highest_seq + 1;
        s_write_slot = (highest_slot + 1) % s_total_slots;
    } else {
        s_next_seq   = 0;
        s_write_slot = 0;
    }
    ESP_LOGI(TAG, "fault log ready: %lu slots, resuming at seq=%lu",
             (unsigned long)s_total_slots, (unsigned long)s_next_seq);
}

static void flash_append(app_log_level_t level, const char *tag, const char *msg)
{
    if (!s_part || !s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    uint32_t slot = s_write_slot;

    // First record of a sector this lap — erase it before writing into it.
    if (slot % RECORDS_PER_SECTOR == 0) {
        uint32_t sector_offset = (slot / RECORDS_PER_SECTOR) * SECTOR_SIZE;
        esp_partition_erase_range(s_part, sector_offset, SECTOR_SIZE);
    }

    flash_record_t rec = { 0 };
    rec.magic        = RECORD_MAGIC;
    rec.seq          = s_next_seq;
    rec.timestamp_ms = esp_log_timestamp();
    rec.level        = (uint8_t)level;
    strncpy(rec.tag, tag, sizeof(rec.tag) - 1);
    strncpy(rec.msg, msg, sizeof(rec.msg) - 1);

    esp_partition_write(s_part, slot * RECORD_SIZE, &rec, RECORD_SIZE);

    s_write_slot = (slot + 1) % s_total_slots;
    s_next_seq++;

    xSemaphoreGive(s_lock);
}

int app_log_read_faults(app_log_fault_record_t *out, int max_out)
{
    if (!s_part || !s_lock || max_out <= 0) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return 0;

    // Collect every valid record, oldest-first by seq. Fault volume is
    // low by design (WARN/ERROR only), so a simple full scan + insertion
    // into the caller's buffer (already seq-ordered since we walk slots in
    // write order starting from the oldest surviving one) is fine.
    int count = 0;
    for (uint32_t i = 0; i < s_total_slots && count < max_out; i++) {
        // Walk starting from the current write position (oldest surviving
        // record lives right after it, since that's the next slot to be
        // overwritten) — this yields oldest-to-newest order directly.
        uint32_t slot = (s_write_slot + i) % s_total_slots;
        flash_record_t rec;
        if (esp_partition_read(s_part, slot * RECORD_SIZE, &rec, RECORD_SIZE) != ESP_OK)
            continue;
        if (rec.magic != RECORD_MAGIC) continue;

        app_log_fault_record_t *o = &out[count++];
        o->timestamp_ms = rec.timestamp_ms;
        o->level        = (app_log_level_t)rec.level;
        strncpy(o->tag, rec.tag, sizeof(o->tag) - 1);
        o->tag[sizeof(o->tag) - 1] = '\0';
        strncpy(o->msg, rec.msg, sizeof(o->msg) - 1);
        o->msg[sizeof(o->msg) - 1] = '\0';
    }

    xSemaphoreGive(s_lock);
    return count;
}

void app_log_clear_faults(void)
{
    if (!s_part || !s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) return;
    esp_partition_erase_range(s_part, 0, s_part->size);
    s_write_slot = 0;
    s_next_seq   = 0;
    xSemaphoreGive(s_lock);
}

// ---- Formatting + emit --------------------------------------------------

static const char *level_str(app_log_level_t level)
{
    switch (level) {
        case APP_LOG_INFO:  return "INFO";
        case APP_LOG_WARN:  return "WARN";
        case APP_LOG_ERROR: return "ERROR";
        case APP_LOG_DEBUG: return "DEBUG";
        default:            return "?";
    }
}

void app_log_emit(app_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (level == APP_LOG_DEBUG && !s_debug_on) return;

    char msg[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    printf("[%s] (%s) (%lu) \"%s\"\n",
           level_str(level), tag, (unsigned long)esp_log_timestamp(), msg);

    if (level == APP_LOG_WARN || level == APP_LOG_ERROR) {
        flash_append(level, tag, msg);
    }
}
