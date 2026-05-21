/**
 * diagnostics.h - Boot-time + runtime introspection helpers.
 *
 * - diagnosticsLogBoot() records the reset reason (powered-on,
 *   watchdog, panic, etc.) and bumps an NVS-backed boot counter so a
 *   deployed device's crash history is visible without serial.
 * - The cached values are exposed via accessors so the ESPUI Status
 *   tab can render them, and so taskCore0 can update the live
 *   diagnostics labels without re-querying NVS each tick.
 */
#pragma once

#include <stdint.h>

// Call once during setup(), AFTER preferences.begin() but before any
// other init that could itself reset the chip.
void diagnosticsLogBoot();

// Cached human-readable reset reason from the most recent boot.
const char* diagnosticsResetReason();

// Number of cold/warm boots this device has gone through (persisted).
uint32_t diagnosticsBootCount();

// Track the most recent upload's HTTP status so the diagnostics tab
// can show "200" / "-1" / etc. Updated from api.cpp.
void diagnosticsRecordUpload(int http_code);
int diagnosticsLastUploadCode();

// Uptime formatted as "HHh MMm SSs" into the provided buffer.
void diagnosticsFormatUptime(char* buf, size_t buf_len);
