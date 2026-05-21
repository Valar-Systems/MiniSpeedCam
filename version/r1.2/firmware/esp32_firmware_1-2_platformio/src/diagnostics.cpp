#include "diagnostics.h"

#include <Arduino.h>
#include <esp_system.h>

#include "variables.h"  // preferences

static const char* s_reset_reason = "UNKNOWN";
static uint32_t s_boot_count = 0;
static int s_last_upload_code = 0;

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

void diagnosticsLogBoot() {
  s_reset_reason = resetReasonStr(esp_reset_reason());
  s_boot_count = preferences.getULong("boot_count", 0) + 1;
  preferences.putULong("boot_count", s_boot_count);
  Serial.printf("[boot] reason=%s count=%lu\n",
                s_reset_reason,
                (unsigned long)s_boot_count);
}

const char* diagnosticsResetReason() { return s_reset_reason; }
uint32_t diagnosticsBootCount() { return s_boot_count; }

void diagnosticsRecordUpload(int http_code) { s_last_upload_code = http_code; }
int diagnosticsLastUploadCode() { return s_last_upload_code; }

void diagnosticsFormatUptime(char* buf, size_t buf_len) {
  unsigned long secs = millis() / 1000;
  unsigned long hours = secs / 3600;
  unsigned long mins  = (secs / 60) % 60;
  unsigned long s     = secs % 60;
  snprintf(buf, buf_len, "%luh %02lum %02lus", hours, mins, s);
}
