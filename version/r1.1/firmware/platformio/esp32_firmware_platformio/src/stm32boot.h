/**
 * stm32boot.h - Reflash the STM32 from the ESP32 over UART (AN3155).
 *
 * The ESP drives the STM32's system (ROM) bootloader over the SAME UART1 link
 * it normally uses for the speed protocol:
 *   - STM32_BOOT0_PIN (GPIO14) -> STM32 BOOT0  (HIGH = enter ROM bootloader)
 *   - STM32_RESET_PIN (GPIO47) -> STM32 NRST
 * The ESP↔STM link is on STM32 USART1 (PA9/PA10), which is exactly what the
 * F301 ROM bootloader speaks, so no extra wiring beyond BOOT0 is needed.
 *
 * For the bootloader session UART1 is reconfigured to 115200 8E1 (the bootloader
 * autobauds off the 0x7F sync byte and uses even parity), then restored to the
 * app's 1,000,000 8N1 on exit.
 *
 * IMPORTANT: while a flash is in progress Core 0 owns UART1 for the bootloader.
 * The caller MUST stop taskCore1 from polling the radar over the same UART
 * (see stm_flash_busy) for the whole stm32FlashImage() call.
 *
 * The STM32's bootloader lives in ROM and can't be erased, so a failed flash is
 * always recoverable: re-enter the bootloader (BOOT0 + reset) and retry.
 */
#pragma once

#define STM_BL_ACK   0x79
#define STM_BL_NACK  0x1F
#define STM_FLASH_BASE  0x08000000UL
#define STM_WRITE_CHUNK 256   // AN3155 Write Memory max is 256 bytes/frame

// Read one byte from UART1 with a timeout; -1 on timeout.
static int stmBlReadByte(uint32_t timeout_ms) {
  uint32_t start = millis();
  while ((millis() - start) < timeout_ms) {
    if (Serial1.available()) return (uint8_t)Serial1.read();
    delay(1);  // yield: bootloader waits (mass-erase/write ACKs) can run seconds; a
               // bare spin would starve IDLE0 and trip the task watchdog. Costs <=1ms
               // of reply latency, negligible against the multi-second flash.
  }
  return -1;
}

static bool stmBlWaitAck(uint32_t timeout_ms) {
  return stmBlReadByte(timeout_ms) == STM_BL_ACK;
}

static void stmBlFlush() {
  while (Serial1.available()) Serial1.read();
}

// Send a command byte + its complement, wait for ACK (AN3155 framing).
static bool stmBlCmd(uint8_t cmd) {
  uint8_t buf[2] = { cmd, (uint8_t)(cmd ^ 0xFF) };
  Serial1.write(buf, 2);
  return stmBlWaitAck(500);
}

// BOOT0 high + reset into the ROM bootloader, switch UART to 115200 8E1, sync.
static bool stmBlEnter() {
  pinMode(STM32_BOOT0_PIN, OUTPUT);
  digitalWrite(STM32_BOOT0_PIN, HIGH);   // select system bootloader
  delay(5);
  digitalWrite(STM32_RESET_PIN, LOW);    // pulse NRST
  delay(20);
  digitalWrite(STM32_RESET_PIN, HIGH);
  delay(60);                             // let the bootloader come up

  Serial1.end();
  Serial1.begin(115200, SERIAL_8E1, RX_GPIO, TX_GPIO);  // bootloader: even parity
  delay(10);
  stmBlFlush();

  // Autobaud sync: send 0x7F once; the bootloader replies ACK. A retry that hits
  // an already-synced bootloader gets NACK -- still proof it's alive and ready.
  for (int i = 0; i < 5; i++) {
    Serial1.write((uint8_t)0x7F);
    int b = stmBlReadByte(300);
    if (b == STM_BL_ACK || b == STM_BL_NACK) return true;
  }
  return false;
}

// BOOT0 low + reset to run the freshly-flashed app, restore the app UART.
static void stmBlExit() {
  digitalWrite(STM32_BOOT0_PIN, LOW);    // boot from flash
  delay(5);
  digitalWrite(STM32_RESET_PIN, LOW);
  delay(20);
  digitalWrite(STM32_RESET_PIN, HIGH);

  Serial1.end();
  Serial1.begin(1000000, SERIAL_8N1, RX_GPIO, TX_GPIO);  // restore app protocol
  delay(50);
  stmBlFlush();
}

// Mass-erase all flash. Tries Extended Erase (0x44, used by all STM32F3 ROM
// bootloaders); falls back to legacy Erase (0x43) for older parts.
static bool stmBlEraseAll(String& err) {
  uint8_t ext[2] = { 0x44, 0xBB };
  Serial1.write(ext, 2);
  if (stmBlWaitAck(500)) {
    uint8_t mass[3] = { 0xFF, 0xFF, 0x00 };  // 0xFFFF = global mass erase, checksum 0x00
    Serial1.write(mass, 3);
    if (stmBlWaitAck(30000)) return true;    // erase can take seconds
    err = "ext-erase exec NACK";
    return false;
  }
  uint8_t leg[2] = { 0x43, 0xBC };
  Serial1.write(leg, 2);
  if (stmBlWaitAck(500)) {
    uint8_t glob[2] = { 0xFF, 0x00 };        // global erase
    Serial1.write(glob, 2);
    if (stmBlWaitAck(30000)) return true;
    err = "erase exec NACK";
    return false;
  }
  err = "erase cmd NACK";
  return false;
}

// Write up to 256 bytes at `addr` (len must be a multiple of 4). AN3155 0x31.
static bool stmBlWriteChunk(uint32_t addr, const uint8_t* data, uint16_t len) {
  if (!stmBlCmd(0x31)) return false;
  uint8_t a[5];
  a[0] = (addr >> 24) & 0xFF;
  a[1] = (addr >> 16) & 0xFF;
  a[2] = (addr >> 8) & 0xFF;
  a[3] = addr & 0xFF;
  a[4] = a[0] ^ a[1] ^ a[2] ^ a[3];
  Serial1.write(a, 5);
  if (!stmBlWaitAck(500)) return false;

  uint8_t n = (uint8_t)(len - 1);          // wire count = bytes - 1
  uint8_t cks = n;
  for (uint16_t i = 0; i < len; i++) cks ^= data[i];
  Serial1.write(n);
  Serial1.write(data, len);
  Serial1.write(cks);
  return stmBlWaitAck(2000);               // flash program time
}

// Flash a full image to 0x08000000. Returns false (with err set) on any step.
// The caller must hold stm_flash_busy across this call (UART1 hand-off).
bool stm32FlashImage(const uint8_t* img, size_t len, String& err) {
  if (!stmBlEnter()) { err = "no bootloader response"; stmBlExit(); return false; }
  if (!stmBlEraseAll(err)) { stmBlExit(); return false; }

  uint8_t chunk[STM_WRITE_CHUNK];
  for (size_t off = 0; off < len; off += STM_WRITE_CHUNK) {
    size_t n = len - off;
    if (n > STM_WRITE_CHUNK) n = STM_WRITE_CHUNK;
    memcpy(chunk, img + off, n);
    size_t padded = (n + 3u) & ~((size_t)3u);     // word-align; pad with 0xFF (erased value)
    for (size_t i = n; i < padded; i++) chunk[i] = 0xFF;
    if (!stmBlWriteChunk(STM_FLASH_BASE + off, chunk, (uint16_t)padded)) {
      err = "write NACK @ 0x" + String((unsigned long)(STM_FLASH_BASE + off), HEX);
      stmBlExit();
      return false;
    }
  }

  stmBlExit();
  return true;
}
