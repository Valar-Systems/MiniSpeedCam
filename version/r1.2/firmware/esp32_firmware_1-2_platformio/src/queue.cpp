#include "queue.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "api.h"
#include "diagnostics.h"
#include "variables.h"

namespace {

constexpr const char* kQueueDir = "/queue";

// Magic + version so we can evolve the on-disk format without bricking
// already-queued events on upgrade.
constexpr uint32_t kFileMagic   = 0x4d534336;  // 'MSC6'
constexpr uint16_t kFileVersion = 1;

#pragma pack(push, 1)
struct FileHeader {
  uint32_t magic;        // kFileMagic
  uint16_t version;      // kFileVersion
  uint16_t reserved;     // 0; reserved for future flags
  int32_t  speed;        // signed because the firmware uses int speeds
  int64_t  epoch;        // event timestamp (0 if NTP was unsynced)
  uint16_t camera_id_len;// bytes of camera_id string that follow header
  uint32_t jpeg_len;     // bytes of JPEG that follow camera_id
};
#pragma pack(pop)

bool g_mounted = false;
uint32_t g_seq = 0;  // monotonic filename counter

String filenameForSeq(uint32_t seq) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%s/%08lu.bin", kQueueDir, (unsigned long)seq);
  return String(buf);
}

// Find the largest existing sequence number under kQueueDir so a new
// enqueue can append rather than collide on first boot after a crash.
uint32_t scanHighestSeq() {
  uint32_t highest = 0;
  File dir = LittleFS.open(kQueueDir);
  if (!dir || !dir.isDirectory()) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    // Filenames are "/queue/NNNNNNNN.bin". The library returns the
    // basename through name(), but some versions include the leading
    // slash -- handle either by scanning for the last '/'.
    const char* name = f.name();
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    uint32_t n = (uint32_t)strtoul(base, nullptr, 10);
    if (n > highest) highest = n;
  }
  return highest;
}

}  // namespace

bool queueInit() {
  // LittleFS shares the SPIFFS partition slot from default_8MB.csv;
  // begin(true) formats on first boot when the partition is empty.
  if (!LittleFS.begin(true)) {
    Serial.println("queue: LittleFS mount failed");
    g_mounted = false;
    return false;
  }
  g_mounted = true;
  if (!LittleFS.exists(kQueueDir)) {
    LittleFS.mkdir(kQueueDir);
  }
  g_seq = scanHighestSeq();
  Serial.printf("queue: ready (highest seq=%lu, pending=%u)\n",
                (unsigned long)g_seq, (unsigned)queuePendingCount());
  return true;
}

bool queueEnqueueEvent(int speed,
                      long long epoch,
                      const String& camera_id,
                      const uint8_t* jpeg,
                      size_t jpeg_len) {
  if (!g_mounted) {
    Serial.println("queue: not mounted; dropping event");
    return false;
  }
  if (jpeg == nullptr || jpeg_len == 0) {
    Serial.println("queue: refusing to enqueue empty payload");
    return false;
  }

  // Bail out before we fill the FS to the point that LittleFS can't
  // perform garbage collection. Conservative threshold: keep 64 KB
  // free at all times.
  size_t total = LittleFS.totalBytes();
  size_t used  = LittleFS.usedBytes();
  if (total > 0 && (total - used) < (jpeg_len + 4096)) {
    Serial.printf("queue: insufficient space (free=%u, need=%u); dropping\n",
                  (unsigned)(total - used), (unsigned)jpeg_len);
    return false;
  }

  uint32_t seq = ++g_seq;
  String path = filenameForSeq(seq);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("queue: open(%s) for write failed\n", path.c_str());
    return false;
  }

  FileHeader hdr{};
  hdr.magic = kFileMagic;
  hdr.version = kFileVersion;
  hdr.speed = (int32_t)speed;
  hdr.epoch = (int64_t)epoch;
  hdr.camera_id_len = (uint16_t)camera_id.length();
  hdr.jpeg_len = (uint32_t)jpeg_len;

  bool ok = f.write((const uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr);
  if (ok && hdr.camera_id_len > 0) {
    ok = f.write((const uint8_t*)camera_id.c_str(), hdr.camera_id_len) == hdr.camera_id_len;
  }
  if (ok) {
    ok = f.write(jpeg, jpeg_len) == jpeg_len;
  }
  f.close();
  if (!ok) {
    Serial.printf("queue: short write for %s; removing partial\n", path.c_str());
    LittleFS.remove(path);
    return false;
  }
  Serial.printf("queue: stored %s (speed=%d, jpeg=%u bytes)\n",
                path.c_str(), speed, (unsigned)jpeg_len);
  return true;
}

size_t queuePendingCount() {
  if (!g_mounted) return 0;
  size_t count = 0;
  File dir = LittleFS.open(kQueueDir);
  if (!dir || !dir.isDirectory()) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) count++;
  }
  return count;
}

namespace {

// Try to upload one queued file; return true on success (file removed),
// false otherwise (file left in place).
bool tryDrainOne(const String& path) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) {
    Serial.printf("queue: open(%s) for read failed; removing\n", path.c_str());
    LittleFS.remove(path);
    return false;
  }

  FileHeader hdr{};
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic != kFileMagic ||
      hdr.version != kFileVersion) {
    Serial.printf("queue: %s has bad header; removing\n", path.c_str());
    f.close();
    LittleFS.remove(path);
    return false;
  }

  String camera_id;
  if (hdr.camera_id_len > 0) {
    camera_id.reserve(hdr.camera_id_len + 1);
    for (uint16_t i = 0; i < hdr.camera_id_len; ++i) {
      camera_id += (char)f.read();
    }
  }

  // Pull the JPEG into PSRAM so we can hand it to the streaming
  // uploader. ps_malloc returns nullptr if PSRAM is exhausted; in
  // that case we leave the file in place for a future retry rather
  // than dropping the event.
  uint8_t* buf = (uint8_t*)heap_caps_malloc(hdr.jpeg_len, MALLOC_CAP_SPIRAM);
  if (!buf) {
    Serial.printf("queue: out of PSRAM for %s (%u bytes); will retry later\n",
                  path.c_str(), (unsigned)hdr.jpeg_len);
    f.close();
    return false;
  }
  size_t got = f.read(buf, hdr.jpeg_len);
  f.close();
  if (got != hdr.jpeg_len) {
    Serial.printf("queue: %s truncated; removing\n", path.c_str());
    free(buf);
    LittleFS.remove(path);
    return false;
  }

  int http_code = apiUploadEventFromMemory((int)hdr.speed,
                                           (long long)hdr.epoch,
                                           camera_id,
                                           buf, hdr.jpeg_len);
  free(buf);
  diagnosticsRecordUpload(http_code);

  if (http_code >= 200 && http_code < 300) {
    LittleFS.remove(path);
    Serial.printf("queue: %s delivered (HTTP %d)\n", path.c_str(), http_code);
    return true;
  }
  Serial.printf("queue: %s deferred (HTTP %d)\n", path.c_str(), http_code);
  return false;
}

}  // namespace

void queueDrain() {
  if (!g_mounted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Collect filenames first so iteration is stable against deletes.
  File dir = LittleFS.open(kQueueDir);
  if (!dir || !dir.isDirectory()) return;

  // We sort by filename (which is the zero-padded sequence) so events
  // are uploaded in capture order. Cap the batch so a huge backlog
  // doesn't starve the rest of the system.
  constexpr size_t kMaxBatch = 8;
  String batch[kMaxBatch];
  size_t n = 0;
  for (File f = dir.openNextFile(); f && n < kMaxBatch; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    String name = f.name();
    // openNextFile() inside arduino-esp32 LittleFS returns the basename
    // (no leading '/'); prepend the directory for the absolute path
    // when needed.
    if (name.indexOf('/') < 0) {
      String full = String(kQueueDir) + "/" + name;
      batch[n++] = full;
    } else {
      batch[n++] = name;
    }
  }

  // Simple insertion sort -- batch is small.
  for (size_t i = 1; i < n; ++i) {
    String key = batch[i];
    size_t j = i;
    while (j > 0 && batch[j - 1] > key) {
      batch[j] = batch[j - 1];
      j--;
    }
    batch[j] = key;
  }

  for (size_t i = 0; i < n; ++i) {
    if (!tryDrainOne(batch[i])) {
      // First failure stops the drain so we don't hammer a broken
      // endpoint or burn battery on a hopeless retry storm.
      return;
    }
  }
}
