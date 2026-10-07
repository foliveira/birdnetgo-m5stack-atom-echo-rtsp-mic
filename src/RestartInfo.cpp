#include "RestartInfo.h"

#include <Arduino.h>
#include <esp_core_dump.h>
#include <esp_system.h>

#define RESTART_NOTE_MAGIC 0x52534E54u   // "RSNT"

RTC_NOINIT_ATTR static uint32_t noteMagic;
RTC_NOINIT_ATTR static char note[40];

static char reason[64] = "unknown";
static char crash[200] = "";

static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power on";
        case ESP_RST_EXT:       return "reset pin";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "crash";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout (supply voltage dropped)";
        case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
        default:                return "unknown";
    }
}

void restartInfoInit() {
    const esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_SW && noteMagic == RESTART_NOTE_MAGIC) {
        note[sizeof(note) - 1] = '\0';
        strlcpy(reason, note, sizeof(reason));
    } else {
        strlcpy(reason, resetReasonName(r), sizeof(reason));
    }
    noteMagic = 0;

    // The panic handler (crashes and watchdog timeouts) writes a core dump to the coredump
    // partition. Its summary is only meaningful right after such a reset: an older dump stays in
    // flash until the next crash overwrites it.
    if (r != ESP_RST_PANIC && r != ESP_RST_INT_WDT && r != ESP_RST_TASK_WDT && r != ESP_RST_WDT) return;
    esp_core_dump_summary_t* s = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!s) return;
    if (esp_core_dump_get_summary(s) == ESP_OK) {
        s->exc_task[sizeof(s->exc_task) - 1] = '\0';
        snprintf(reason, sizeof(reason), "%s in task %s at 0x%08x", resetReasonName(r), s->exc_task,
                 (unsigned)s->exc_pc);
        int n = snprintf(crash, sizeof(crash), "task %s, PC 0x%08x, backtrace:", s->exc_task, (unsigned)s->exc_pc);
        const uint32_t depth = s->exc_bt_info.depth < 8 ? s->exc_bt_info.depth : 8;   // fits one log line
        for (uint32_t i = 0; i < depth && n > 0 && n < (int)sizeof(crash) - 12; i++) {
            n += snprintf(crash + n, sizeof(crash) - n, " 0x%08x", (unsigned)s->exc_bt_info.bt[i]);
        }
        if (s->exc_bt_info.corrupted && n > 0 && n < (int)sizeof(crash) - 12) {
            snprintf(crash + n, sizeof(crash) - n, " (corrupted)");
        }
    }
    free(s);
}

const char* restartReason() { return reason; }

const char* restartCrashDetails() { return crash; }

void restartWithNote(const char* why) {
    strlcpy(note, why, sizeof(note));
    noteMagic = RESTART_NOTE_MAGIC;
    Serial.flush();
    esp_restart();
}
