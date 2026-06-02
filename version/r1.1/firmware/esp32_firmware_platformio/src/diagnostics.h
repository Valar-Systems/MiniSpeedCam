/**
 * diagnostics.h - Boot-time + runtime introspection helpers.
 *
 * Header-only port of r1.2's diagnostics module (minus the offline-queue
 * depth, which r1.1 doesn't have). Records the reset reason and an
 * NVS-backed boot counter, tracks the most recent upload's HTTP status, and
 * formats uptime for the ESPUI Status tab.
 *
 * diagnosticsLogBoot() must be called once in setup(), AFTER
 * preferences.begin() (it reads/writes the shared `preferences` instance).
 */
#pragma once

#include <Arduino.h>
#include <esp_system.h>

static const char* g_reset_reason = "UNKNOWN";  // cached human-readable reset cause
static uint32_t g_boot_count = 0;                // persisted across reboots
static int g_last_upload_code = 0;               // most recent upload HTTP status

static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

/**
 * Capture the reset reason and bump the persisted boot counter.
 * Call once in setup() after preferences.begin().
 */
void diagnosticsLogBoot() {
  g_reset_reason = resetReasonStr(esp_reset_reason());
  g_boot_count = preferences.getULong("boot_count", 0) + 1;
  preferences.putULong("boot_count", g_boot_count);
  Serial.printf("[boot] reason=%s count=%lu\n", g_reset_reason, (unsigned long)g_boot_count);
}

const char* diagnosticsResetReason() { return g_reset_reason; }
uint32_t diagnosticsBootCount() { return g_boot_count; }

// Record the last upload's HTTP status (called from sendUpload on Core 0).
void diagnosticsRecordUpload(int http_code) { g_last_upload_code = http_code; }
int diagnosticsLastUploadCode() { return g_last_upload_code; }

// Format uptime as "HHh MMm SSs" into the provided buffer.
void diagnosticsFormatUptime(char* buf, size_t buf_len) {
  unsigned long secs = millis() / 1000;
  unsigned long hours = secs / 3600;
  unsigned long mins = (secs / 60) % 60;
  unsigned long s = secs % 60;
  snprintf(buf, buf_len, "%luh %02lum %02lus", hours, mins, s);
}
