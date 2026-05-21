/**
 * radar.h - UART protocol with the STM32 (which drives the CDM324 radar).
 *
 * The STM32 performs the FFT on the amplified CDM324 signal and exposes
 * a tiny ASCII protocol over UART1 (1,000,000 baud, 8N1):
 *   - Send 'm' to request the latest speed in MPH * 10.
 *   - Send 'k' to request the latest speed in KPH * 10.
 *   - The STM32 replies with an ASCII float terminated by newline.
 *
 * On reset, the STM32 emits a banner string which we drain in
 * issue_cdm324_reset() so it doesn't pollute the first speed query.
 */
#pragma once

/**
 * Query the STM32 for the most recent speed sample.
 *
 * @param kmh  true = request KPH, false = request MPH.
 * @return     Speed in the requested units. Returns 0 if the STM32 did
 *             not have data ready when polled.
 */
float get_speed(bool kmh);

/**
 * Reset the STM32 and consume its boot banner.
 *
 * Drives STM32_RESET_PIN low for 20ms, then reads bytes off UART1 until
 * a newline (or the buffer fills) so the next get_speed() starts from
 * a clean RX queue. The banner is echoed on Serial for debugging.
 */
void issue_cdm324_reset();
