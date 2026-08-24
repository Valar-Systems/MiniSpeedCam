/* USER CODE BEGIN Header */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "user/defines.h"
#include "user/ascii_lib.h"
#include "arm_math.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

// Firmware version reported to the ESP32 over UART on the 'v' command. The
// ESP-driven OTA flow reads this to decide whether this MCU needs reflashing.
// Bump on every STM32 firmware release. Build-flag overridable so release CI can
// stamp it from the git tag (-DSTM_FW_VERSION=\"1.0.4\") and keep the published
// manifest's stm_version consistent with the binary it ships.
#ifndef STM_FW_VERSION
#define STM_FW_VERSION "1.0.4"
#endif

#define DOPPLER_Pin GPIO_PIN_0
#define DOPPLER_GPIO_Port GPIOB

/* --- Noise-robustness tuning (shared 5V rail / ESP32 WiFi interference) --- */
/* ADC sample rate, Hz (64MHz/128/(1.5+12.5)); used to convert a bin to Hz. */
#define ANALOG_SAMPLE_RATE_HZ   35714.0f
/* Minimum peak-to-noise-floor ratio for a frame to count as a detection.
 * Measured floor: idle/noise peaks ride at a ratio of ~6-13 (snrx10 60-130),
 * while a real Doppler tone (2048Hz fork) towers at ~70-195 (snrx10 700-1950).
 * 15 sits in the wide gap: idle reads 0, real targets pass easily. Tunable. */
#define ANALOG_MIN_SNR          15.0f
/* Transient rejection (impulsive WiFi-burst noise): count the samples that
 * deviate more than ANALOG_TRANSIENT_SIGMA from the frame mean, and only
 * discard the frame once at least ANALOG_TRANSIENT_MIN_BAD of them are out. A
 * clean -- or even a clipping -- tone stays within ~1.4 sigma so it is never
 * counted. Deliberately lenient (a single stray sample no longer nukes a
 * frame): the per-frame SNR test above is the main noise gate. */
#define ANALOG_TRANSIENT_SIGMA  6.0f
#define ANALOG_TRANSIENT_MIN_BAD 32
/* Lowest FFT bin considered a real target. Bins 1-2 sit right against DC and
 * catch huge impact/drift artifacts (seen at mag >400k), so the search starts
 * above them. remove_low_freqs raises this further to ~10 (a min-speed gate). */
#define ANALOG_MIN_BIN          3

/* Detection debounce: the minimum number of *valid* (non-zero) detections that
 * must be present in the 32-frame rolling window before a non-zero speed is
 * emitted. The median deliberately ignores no-target zeros (so a brief real
 * pass isn't swallowed -- see analog_buffer_median), but that alone lets a
 * SINGLE false detection latch the output for the whole ~0.9s window: e.g. one
 * transient harmonic off a vibrating / rattling reflector that momentarily
 * clears the SNR gate. Requiring several detections first rejects such
 * isolated / transient spikes while still catching a real target, which dwells
 * in the beam for many ~29ms frames and fills the window easily. Raise toward
 * the window size (32) for more aggressive vibration rejection; lower it to
 * catch very brief passes. */
#define ANALOG_MIN_VALID_DETECTIONS 5

/* --- Doppler-to-speed conversion (24.125 GHz CDM324) --------------------- *
 * A target moving at v produces a Doppler shift f_d = 2*v*f0/c. The FFT peak
 * bin is first turned into that frequency in Hz (bin * ANALOG_SAMPLE_RATE_HZ /
 * N, see analog_compute_fft_on_cplted_sequence), then into speed with the
 * factors below. BOTH unit factors are derived from ONE physical constant so
 * the 'm' and 'k' replies can never drift apart -- they must stay in the exact
 * 1.60934 mph<->km/h ratio. The previous hand-tuned literals (0.1449275 mph,
 * 0.2262295 kph) did NOT: they implied two different radar frequencies (23.14
 * vs 23.86 GHz) and over-reported by +4.3% / +1.1%, always high (the accusatory
 * direction). NOTE: this changes every reported speed by 1-4%; re-verify on the
 * tuning-fork bench (a 2048 Hz tone must read 28.5 mph / 45.8 km/h, and the two
 * units must agree through the 1.609 conversion) before releasing. */
#define CDM324_F0_HZ            24.125e9f
#define SPEED_OF_LIGHT_MPS      299792458.0f
#define DOPPLER_HZ_PER_MPS      (2.0f * CDM324_F0_HZ / SPEED_OF_LIGHT_MPS)   /* 160.945 */
#define MPS_PER_MPH             0.44704f
#define MPS_PER_KPH             (1.0f / 3.6f)
/* Reply carries speed*10, so factor = 10 / (Doppler Hz per unit). */
#define FFT_HZ_TO_MPH_X10       (10.0f / (DOPPLER_HZ_PER_MPS * MPS_PER_MPH))  /* 0.138988 */
#define FFT_HZ_TO_KPH_X10       (10.0f / (DOPPLER_HZ_PER_MPS * MPS_PER_KPH))  /* 0.223680 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

// Define to enable current transient test
//#define CUR_TRANSIENT_TEST

volatile uint16_t analog_result_buffer1[1024];
volatile uint16_t analog_result_buffer2[1024];
/* Pointer objects themselves are volatile (not just the pointed-to data): they
 * are reassigned in the DMA ISR (analog_trigger_conversion) and read in the main
 * loop, so the compiler must re-load them each access, not cache across the loop. */
volatile uint16_t * volatile analog_cur_adc_dest_buf_pt;
volatile uint16_t * volatile analog_cplt_adc_dest_buf_pt;
volatile BOOL analog_fdma_transfer_started;
volatile BOOL analog_adc_train_done_flag;
arm_rfft_fast_instance_f32 analog_fft;
float32_t analog_temp_float_array[1024];
float32_t analog_rfft_output[1024];
uint16_t analog_last_peak_indexes[32];
uint16_t analog_cur_peak_fill_idx = 0;

/* Per-frame diagnostics, dumped over USART2 (CH340 debug header) when enabled
 * (lets us see why a frame was rejected without an external probe). */
volatile uint16_t analog_dbg_peak_bin = 0;
volatile uint32_t analog_dbg_peak_mag = 0;
volatile uint32_t analog_dbg_floor_mean = 0;
volatile uint16_t analog_dbg_bad_count = 0;
volatile uint8_t analog_dbg_dropped = 0;
volatile uint8_t analog_dbg_blanked = 0; /* frame discarded due to WiFi-busy line */

/* ADC/DMA overrun detector. HAL_ADC_ConvCpltCallback sets this if the main loop
 * had not yet consumed the previous frame (analog_adc_train_done_flag still TRUE)
 * when the next DMA train completed and the buffers were swapped: the frame the
 * main loop is about to read is being overwritten by the ADC DMA -- a torn splice
 * of two frames -- so the FFT path discards it (holds the median) instead of
 * emitting a false peak from it. analog_overrun_count trends the rate. */
volatile uint8_t  analog_overrun_flag = 0;
volatile uint32_t analog_overrun_count = 0;

/* USART1 (ESP32 link) interrupt-driven RX ring buffer. The ESP polls for speed
 * with single command bytes; the F3 USART has no RX FIFO, so a stray noise byte
 * arriving during the ~2.4ms FFT would overrun and (unhandled) stall reception.
 * Taking bytes in by interrupt into this buffer makes command RX robust. */
#define UART1_RX_BUF_SIZE 32u
volatile uint8_t uart1_rx_buf[UART1_RX_BUF_SIZE];
volatile uint16_t uart1_rx_head = 0; /* written by ISR */
volatile uint16_t uart1_rx_tail = 0; /* read by main loop */
volatile uint8_t uart1_rx_byte = 0;  /* landing byte for HAL_UART_Receive_IT */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

// Debug
#define PUTCHAR_ESP_PROTOTYPE int __io_putchar(int ch)

static void debug_dma_output_buffer(uint8_t *buffer, uint16_t size);
//static void debug_printf(const char *fmt, ...);
//static void debug_print_string(char* string);
static char debug_get_char_from_uart(void);
static void uart_reply_speed(uint16_t value, uint16_t mag, uint16_t snr);

// Analog
static uint16_t analog_compute_fft_on_cplted_sequence(BOOL remove_low_freqs);
static uint16_t analog_buffer_median(void);
static void analog_output_current_fft_to_uart(uint16_t nb_bins);
static BOOL analog_get_and_clear_adc_measurement_done(void);
static void analog_output_conversion_buffer_to_uart(void);
static void analog_trigger_conversion(void);
static void analog_init(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

	/* USER CODE BEGIN 1 */
	/* USER CODE END 1 */

	/* MCU Configuration--------------------------------------------------------*/

	/* Reset of all peripherals, Initializes the Flash interface and the Systick. */
	HAL_Init();

	/* USER CODE BEGIN Init */

	/* USER CODE END Init */

	/* Configure the system clock */
	SystemClock_Config();

	/* USER CODE BEGIN SysInit */

	/* USER CODE END SysInit */

	/* Initialize all configured peripherals */
	MX_GPIO_Init();
	MX_DMA_Init();
	MX_ADC1_Init();
	MX_FATFS_Init();
	MX_USART1_UART_Init();
	MX_USART2_UART_Init();
	/* USER CODE BEGIN 2 */

	BOOL output_debug_enabled = FALSE;

	BOOL remove_low_freqs = FALSE;
	uint16_t last_fft_return = 0;
	/* Per-frame diagnostic dump (USART2). OFF in production so it never steals
	 * ~6ms/frame from the sampling loop (which raises frame-overrun risk) or talks
	 * over the ESP32 link; a bench build enables it with -DDIAG_DEFAULT_ON, and the
	 * 'd' command still toggles it when debug commands are compiled in. */
#ifdef DIAG_DEFAULT_ON
	BOOL diag_enabled = TRUE;
#else
	BOOL diag_enabled = FALSE;
#endif
	uint16_t diag_counter = 0;

#ifdef CUR_TRANSIENT_TEST
  	uint16_t temp_counter = 0;
  #endif

	printf("Connection Successful! \r\n");

	printf("CDM324-V2 fw v%d.%d, compiled %s %s\r\n", FW_MAJOR, FW_MINOR,
	__DATE__, __TIME__);

	/* Diagnostics go out USART2 (PA2/PA3, the CH340 debug port) at 115200, so
	 * they don't collide with the ESP32 link on USART1. This boot line confirms
	 * the CH340 path before any radar signal is present. */
	{
		const char dbg_hello[] =
				"DBG diagnostics active on USART2 @115200\r\n";
		HAL_UART_Transmit(&huart2, (uint8_t*) dbg_hello,
				(uint16_t) (sizeof(dbg_hello) - 1), 0xFFFF);
	}

	/* Analog input init */
	analog_init();

	/* Trigger analog conversions */
	analog_trigger_conversion();

	/* Arm interrupt-driven reception of ESP32 command bytes on USART1. */
	HAL_UART_Receive_IT(&huart1, (uint8_t*) &uart1_rx_byte, 1);

	/* Start the independent watchdog (~2s). Started here, after the (possibly
	 * slow) peripheral/FatFs init, so a long boot can't trip it; refreshed at
	 * the top of the loop below. LSI ~40kHz / prescaler 64 = 625Hz, reload
	 * 1250 -> ~2.0s. If the loop ever wedges (stuck UART TX, ADC stall) the MCU
	 * resets itself instead of going deaf. Once started it cannot be stopped
	 * except by reset; the bootloader doesn't run app code, so flashing is
	 * unaffected. */
	IWDG->KR = 0x5555;   /* enable write access to PR/RLR */
	IWDG->PR = 0x04;     /* prescaler /64 */
	IWDG->RLR = 1250;    /* reload value -> ~2.0s timeout */
	IWDG->KR = 0xAAAA;   /* refresh */
	IWDG->KR = 0xCCCC;   /* start the watchdog */

	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */
	while (1) {

		/* Sequence of ADC measurements complete? */
		if (analog_get_and_clear_adc_measurement_done() != FALSE) {
			/* Pet the watchdog ONLY when a frame actually completed. Every useful
			 * action below (FFT, the PA5 wake line, and the 'm'/'k'/'v' UART
			 * replies) is gated on this flag, so an unconditional pet at the top
			 * of the loop would keep feeding the dog even if the ADC/DMA pipeline
			 * stalled (DMA/overrun error with no re-arm) -- leaving the MCU
			 * silently deaf with the watchdog never firing. Frames complete every
			 * ~29ms regardless of target presence or PA4 blanking (the blank check
			 * is downstream of this flag), versus the ~2s IWDG timeout, so the
			 * >50x margin means a genuine stall self-resets within ~2s while
			 * normal operation can never trip it. */
			IWDG->KR = 0xAAAA;

			/* Debug ADC output buffer ? */
			if (output_debug_enabled != FALSE) {
				analog_output_conversion_buffer_to_uart();
			}

			/* Compute FFT */
			last_fft_return = analog_compute_fft_on_cplted_sequence(
					remove_low_freqs);

			/* Drive the ESP32 "target present" wake line (PA5) every frame from the
			 * just-computed result. A vestigial FFT_CNT_DISP counter (left over from
			 * the old 7-segment display it once throttled) used to update PA5 only
			 * ~1 frame in 11 and from the PREVIOUS frame's value -- delaying the wake
			 * by up to ~315ms of stale data, several metres of car travel before the
			 * ESP woke to capture. Now it tracks the current frame with ~29ms latency. */
			HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
					last_fft_return ? GPIO_PIN_SET : GPIO_PIN_RESET);

			/* Debug FFT output buffer ? */
			if (output_debug_enabled != FALSE) {
				analog_output_current_fft_to_uart(150);
			}

			/* Readable per-frame diagnostics (toggle with 'd'). Throttled so a
			 * 1Mbaud terminal stays legible while watching a test signal.
			 * snrx10 is the peak/floor ratio x10 (detection needs >= 40). */
			if (diag_enabled != FALSE && ++diag_counter >= 5) {
				diag_counter = 0;
				uint32_t snr_x10 = (analog_dbg_floor_mean != 0)
						? (analog_dbg_peak_mag * 10UL / analog_dbg_floor_mean)
						: 0;
				char dbg_buf[128];
				int dbg_len = snprintf(dbg_buf, sizeof(dbg_buf),
						"DBG blank=%u drop=%u bad=%u bin=%u mag=%lu floor=%lu snrx10=%lu spd=%u\r\n",
						(unsigned) analog_dbg_blanked,
						(unsigned) analog_dbg_dropped,
						(unsigned) analog_dbg_bad_count,
						(unsigned) analog_dbg_peak_bin,
						(unsigned long) analog_dbg_peak_mag,
						(unsigned long) analog_dbg_floor_mean,
						(unsigned long) snr_x10, (unsigned) last_fft_return);
				if (dbg_len > 0) {
					HAL_UART_Transmit(&huart2, (uint8_t*) dbg_buf,
							(uint16_t) dbg_len, 0xFFFF);
				}
			}

			/* Commands from UART. Only the query set ('m'/'k'/'v') is honored in
			 * production. On this noise-prone link a single corrupted byte that
			 * decoded as a state-changing command used to silently mutate detection
			 * until the next reset -- e.g. a stray 'h' raised the min-speed floor so
			 * slow (school-zone) cars stopped being detected, with no field-visible
			 * cause. A corrupted 'm'/'k'/'v' is harmless: the reply is checksummed,
			 * so the ESP just rejects it. The debug/config commands are compiled in
			 * only for bench builds (-DENABLE_DEBUG_COMMANDS). */
			char uart_input = debug_get_char_from_uart();
			if (uart_input == 'k' || uart_input == 'm') {
				/* Clamp the raw FFT peak magnitude into a uint16 for the reply:
				 * >>4 keeps the (large) magnitudes in range while preserving
				 * plenty of resolution for the ESP32's proximity gate. A frame
				 * that failed the SNR gate carries no target, so report 0. */
				uint32_t m = (last_fft_return != 0) ? (analog_dbg_peak_mag >> 4) : 0;
				if (m > 65535U) {
					m = 65535U;
				}
				/* Peak/floor ratio x10 (the unit-independent detection quality; a
				 * valid detection needs >= 10*ANALOG_MIN_SNR = 150). Unlike <mag> it
				 * doesn't scale with target size, so the ESP32 can log it as a
				 * per-event confidence and the floor can be trended for EMI. 0 when
				 * the frame failed the gate or the noise floor is unknown. */
				uint32_t snr = ((last_fft_return != 0) && (analog_dbg_floor_mean != 0))
						? (analog_dbg_peak_mag * 10UL / analog_dbg_floor_mean)
						: 0;
				if (snr > 65535U) {
					snr = 65535U;
				}
				float scale = (uart_input == 'k') ? FFT_HZ_TO_KPH_X10 : FFT_HZ_TO_MPH_X10;
				uart_reply_speed((uint16_t) (last_fft_return * scale),
						(uint16_t) m, (uint16_t) snr);
			} else if (uart_input == 'v') {
				/* Report firmware version to the ESP32 (OTA version check). The
				 * "V<ver>" prefix keeps it distinct from a speed reply, which is
				 * "<n>,<n>,<n>*<CK>". Carries the SAME XOR checksum (over the
				 * "V<ver>" payload) the speed replies use, so the ESP can reject a
				 * corrupted version answer instead of driving a wrong OTA decision
				 * on a garbled string -- a real risk on this noisy board. */
				char vbuf[24];
				int vn = snprintf(vbuf, sizeof(vbuf), "V%s", STM_FW_VERSION);
				if (vn < 0) vn = 0;
				if (vn > (int) sizeof(vbuf) - 1) vn = (int) sizeof(vbuf) - 1;  /* truncation guard */
				uint8_t vck = 0;
				for (int i = 0; i < vn; i++) {
					vck ^= (uint8_t) vbuf[i];
				}
				printf("%s*%02X\r\n", vbuf, vck);
			}
#ifdef ENABLE_DEBUG_COMMANDS
			else if (uart_input == 'a') {
				output_debug_enabled = TRUE;
			} else if (uart_input == 's') {
				output_debug_enabled = FALSE;
			} else if (uart_input == 'h') {
				remove_low_freqs = TRUE;
			} else if (uart_input == 'l') {
				remove_low_freqs = FALSE;
			} else if (uart_input == 'd') {
				/* Toggle the readable diagnostic dump. */
				diag_enabled = (diag_enabled == FALSE) ? TRUE : FALSE;
			}
#endif
		}

		/* USER CODE END WHILE */

		/* USER CODE BEGIN 3 */
	}
	/* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };
	RCC_PeriphCLKInitTypeDef PeriphClkInit = { 0 };

	/** Initializes the RCC Oscillators according to the specified parameters
	 * in the RCC_OscInitTypeDef structure.
	 */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
	RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the CPU, AHB and APB buses clocks
	 */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			| RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
		Error_Handler();
	}
	PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1
			| RCC_PERIPHCLK_ADC1;
	PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK1;
	PeriphClkInit.Adc1ClockSelection = RCC_ADC1PLLCLK_DIV128;

	if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
		Error_Handler();
	}
}

/**
 * @brief ADC1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_ADC1_Init(void) {

	/* USER CODE BEGIN ADC1_Init 0 */

	/* USER CODE END ADC1_Init 0 */

	ADC_ChannelConfTypeDef sConfig = { 0 };

	/* USER CODE BEGIN ADC1_Init 1 */

	/* USER CODE END ADC1_Init 1 */

	/** Common config
	 */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
	hadc1.Init.ContinuousConvMode = ENABLE;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.DMAContinuousRequests = ENABLE;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc1.Init.LowPowerAutoWait = DISABLE;
	hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
	if (HAL_ADC_Init(&hadc1) != HAL_OK) {
		Error_Handler();
	}

	/** Configure Regular Channel
	 */
	sConfig.Channel = ADC_CHANNEL_11;
	sConfig.Rank = ADC_REGULAR_RANK_1;
	sConfig.SingleDiff = ADC_SINGLE_ENDED;
	sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
	sConfig.OffsetNumber = ADC_OFFSET_NONE;
	sConfig.Offset = 0;
	if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN ADC1_Init 2 */

	/* USER CODE END ADC1_Init 2 */

}

/**
 * @brief USART1 Initialization Function
 * @param None
 * @retval None
 */
static void MX_USART1_UART_Init(void) {

	/* USER CODE BEGIN USART1_Init 0 */

	/* USER CODE END USART1_Init 0 */

	/* USER CODE BEGIN USART1_Init 1 */

	/* USER CODE END USART1_Init 1 */
	huart1.Instance = USART1;
	huart1.Init.BaudRate = 1000000;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_TX_RX;
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;
	huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN USART1_Init 2 */

	/* USER CODE END USART1_Init 2 */

}

/**
 * @brief USART2 Initialization Function
 * @param None
 * @retval None
 */
static void MX_USART2_UART_Init(void) {

	/* USER CODE BEGIN USART2_Init 0 */

	/* USER CODE END USART2_Init 0 */

	/* USER CODE BEGIN USART2_Init 1 */

	/* USER CODE END USART2_Init 1 */
	huart2.Instance = USART2;
	huart2.Init.BaudRate = 115200;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;
	huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
	huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
	if (HAL_UART_Init(&huart2) != HAL_OK) {
		Error_Handler();
	}
	/* USER CODE BEGIN USART2_Init 2 */

	/* USER CODE END USART2_Init 2 */

}

/**
 * Enable DMA controller clock
 */
static void MX_DMA_Init(void) {

	/* DMA controller clock enable */
	__HAL_RCC_DMA1_CLK_ENABLE();

	/* DMA interrupt init */
	/* DMA1_Channel1_IRQn interrupt configuration */
	HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };
	/* USER CODE BEGIN MX_GPIO_Init_1 */

	/* USER CODE END MX_GPIO_Init_1 */

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);

	/*Configure GPIO pin : PA5 */
	GPIO_InitStruct.Pin = GPIO_PIN_5;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	/* USER CODE BEGIN MX_GPIO_Init_2 */
	/* PA4 <- ESP32 GPIO2: "WiFi TX busy" blanking line. The ESP drives it HIGH
	 * around WiFi bursts (uploads/reconnects) that disturb the shared rail; we
	 * discard any FFT frame seen during that window. Pull-down so it reads
	 * "not busy" while the ESP is booting / the line is floating. */
	GPIO_InitStruct.Pin = GPIO_PIN_4;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLDOWN;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
	/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

///////////
// DEBUG //
///////////
PUTCHAR_ESP_PROTOTYPE {
	/* Place your implementation of fputc here */
	/* e.g. write a character to the USART1 and Loop until the end of transmission */
	HAL_UART_Transmit(&huart1, (uint8_t*) &ch, 1, 0xFFFF);
	//HAL_UART_Transmit(&huart2, (uint8_t*) &ch, 1, 0xFFFF);

	return ch;
}

/*! \fn     debug_dma_output_buffer(uint8_t* buffer, uint16_t size)
 *   \brief  Send a debug buffer over the USART2 (CH340) diagnostics port
 *   \param	buffer	pointer to the buffer
 *   \param	size	buffer size
 *   \note	USART1 has NO TX DMA channel configured (MX_DMA_Init wires only the
 *   		ADC on DMA1_Channel1), so huart1.hdmatx is NULL. The old
 *   		HAL_UART_Transmit_DMA(&huart1,...) therefore either faulted or -- since
 *   		debug_uart_tx_in_progress was never cleared (there is no
 *   		TxCpltCallback) -- made the NEXT call spin forever in the busy-wait
 *   		until the ~2s IWDG reset the MCU. A single stray 'a' byte on the noisy
 *   		ESP link was enough to trigger it. Now a bounded blocking transmit on
 *   		USART2 (where diagnostics belong) is used: it can never wedge the loop
 *   		and never corrupts the USART1 speed protocol. Blocking, not DMA.
 */
void debug_dma_output_buffer(uint8_t *buffer, uint16_t size) {
	/* 500ms timeout is a safety bound only: a 2KB dump is ~178ms at 115200,
	 * comfortably under both this timeout and the ~2s watchdog. */
	HAL_UART_Transmit(&huart2, buffer, size, 500);
}

/*! \fn     debug_get_char_from_uart(void)
 *   \brief  Pop one command byte from the interrupt-fed USART1 RX ring buffer
 *   \return	0 if nothing was received, otherwise the char
 *   \note	The protocol only ever sends ASCII command bytes, never 0x00, so 0
 *   		is unambiguous as "empty". Single-producer (ISR) / single-consumer
 *   		(main) ring, so no critical section is needed.
 */
char debug_get_char_from_uart(void) {
	if (uart1_rx_tail == uart1_rx_head) {
		return 0; /* buffer empty */
	}
	char c = (char) uart1_rx_buf[uart1_rx_tail];
	uart1_rx_tail = (uint16_t) ((uart1_rx_tail + 1u) % UART1_RX_BUF_SIZE);
	return c;
}

/*! \fn     uart_reply_speed(uint16_t value, uint16_t mag, uint16_t snr)
 *   \brief  Reply to an ESP32 speed query as "<value>,<mag>,<snr>*<CK>\r\n"
 *   \param	value	speed in (MPH or KPH) x10
 *   \param	mag		clamped FFT peak magnitude (proximity proxy, ~1/r^4)
 *   \param	snr		peak/floor ratio x10 (unit-independent detection quality)
 *   \note	CK is the XOR of every ASCII char of the "<value>,<mag>,<snr>"
 *   		payload (digits and the commas), printed as 2 hex chars, e.g.
 *   		298,1024,57 -> "298,1024,57*?? \r\n". The magnitude lets the ESP32
 *   		gate captures on target proximity: a distant car returns a weak
 *   		peak while a car on the road towers, even when both pass the SNR
 *   		gate. The SNR is forwarded for per-event confidence/telemetry. The
 *   		checksum lets the ESP reject UART corruption that would otherwise
 *   		stay in-range and slip past its plausibility check (a real risk on
 *   		this electrically noisy board). The trailing field is backward
 *   		compatible: an older ESP parses <value>/<mag> and ignores <snr>.
 *   		Stays human-readable in the debug monitor.
 */
void uart_reply_speed(uint16_t value, uint16_t mag, uint16_t snr) {
	char payload[24];
	int n = snprintf(payload, sizeof(payload), "%u,%u,%u", (unsigned) value,
			(unsigned) mag, (unsigned) snr);
	uint8_t ck = 0;
	for (int i = 0; i < n; i++) {
		ck ^= (uint8_t) payload[i];
	}
	printf("%s*%02X\r\n", payload, ck);
}

/*! \fn     HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
 *   \brief  USART1 RX-complete ISR: stash the byte and re-arm reception
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance == USART1) {
		uint16_t next = (uint16_t) ((uart1_rx_head + 1u) % UART1_RX_BUF_SIZE);
		if (next != uart1_rx_tail) { /* drop the byte if the ring is full */
			uart1_rx_buf[uart1_rx_head] = uart1_rx_byte;
			uart1_rx_head = next;
		}
		HAL_UART_Receive_IT(huart, (uint8_t*) &uart1_rx_byte, 1);
	}
}

/*! \fn     HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
 *   \brief  Clear USART1 errors (overrun/framing/noise from line noise) and
 *           re-arm RX so a single bad byte can't permanently deafen the link.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance == USART1) {
		__HAL_UART_CLEAR_OREFLAG(huart);
		__HAL_UART_CLEAR_FEFLAG(huart);
		__HAL_UART_CLEAR_NEFLAG(huart);
		__HAL_UART_CLEAR_PEFLAG(huart);
		HAL_UART_Receive_IT(huart, (uint8_t*) &uart1_rx_byte, 1);
	}
}

/////////////
// ANALOG //
/////////////

/*! \fn     HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
 *   \brief  Function called by interrupt at the end of the DMA transfer
 *   \param	hadc	Pointer to the adc handle that brought up this interrupt
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
	/* Overrun: the previous frame's done-flag is still set, so the main loop has
	 * not consumed it yet. Swapping buffers now leaves main reading a buffer the
	 * ADC DMA is re-filling from index 0 (a torn splice of two frames). Flag it so
	 * the FFT path discards the frame instead of emitting a false peak from it. */
	if (analog_adc_train_done_flag != FALSE) {
		analog_overrun_flag = 1;
		analog_overrun_count++;
	}
	analog_trigger_conversion();
	analog_adc_train_done_flag = TRUE;
}

/*! \fn     analog_init(void)
 *   \brief  Analog inputs initialization code
 *   \notes	The ADC is configured at 64M/128/(1.5+12.5)=35.7kHz sampling rate (3us sampling time)
 */
void analog_init(void) {

	/* Dual buffer pointing */
	analog_cur_adc_dest_buf_pt = analog_result_buffer1;
	analog_cplt_adc_dest_buf_pt = analog_result_buffer2;
	analog_fdma_transfer_started = FALSE;
	analog_adc_train_done_flag = FALSE;

	/* FFT library initialization */
	arm_rfft_fast_init_f32(&analog_fft, ARRAY_SIZE(analog_result_buffer1));
}

/*! \fn     analog_trigger_conversion(void)
 *   \brief  Trigger ADC conversion train
 */
void analog_trigger_conversion(void) {
	if (analog_cur_adc_dest_buf_pt == analog_result_buffer1) {
		analog_cur_adc_dest_buf_pt = analog_result_buffer2;
		analog_cplt_adc_dest_buf_pt = analog_result_buffer1;
	} else {
		analog_cur_adc_dest_buf_pt = analog_result_buffer1;
		analog_cplt_adc_dest_buf_pt = analog_result_buffer2;
	}

	if (analog_fdma_transfer_started == FALSE) {
		HAL_ADC_Start_DMA(&hadc1, (uint32_t*) analog_cur_adc_dest_buf_pt,
				sizeof(analog_result_buffer1)
						/ sizeof(analog_result_buffer1[0]));
		analog_fdma_transfer_started = TRUE;
	} else {
		/* Rearm DMA transfer */
		HAL_DMA_Start_IT(hadc1.DMA_Handle, (uint32_t) &hadc1.Instance->DR,
				(uint32_t) analog_cur_adc_dest_buf_pt,
				sizeof(analog_result_buffer1)
						/ sizeof(analog_result_buffer1[0]));

		/* Tayoooo */
		SET_BIT(hadc1.Instance->CR, ADC_CR_ADSTART);
	}
}

/*! \fn     analog_get_and_clear_adc_measurement_done(void)
 *   \brief  Get and clear ADC measurement done flag
 */
BOOL analog_get_and_clear_adc_measurement_done(void) {
	volatile BOOL return_bool;

	__disable_irq();
	return_bool = analog_adc_train_done_flag;
	analog_adc_train_done_flag = FALSE;
	__enable_irq();

	return (BOOL) return_bool;
}

/*! \fn     analog_output_conversion_buffer_to_uart(void)
 *   \brief  Does what it says
 */
void analog_output_conversion_buffer_to_uart(void) {
	debug_dma_output_buffer((uint8_t*) analog_cplt_adc_dest_buf_pt,
			sizeof(analog_result_buffer1));
}

/*! \fn     analog_output_current_fft_to_uart(uint16_t nb_bins)
 *   \brief  Does what it says
 *   \param	nb_bins	Number of bins to send
 */
void analog_output_current_fft_to_uart(uint16_t nb_bins) {
	debug_dma_output_buffer((uint8_t*) analog_temp_float_array,
			nb_bins * sizeof(float32_t));
}

/*! \fn     analog_buffer_median(void)
 *   \brief  Median of the *valid* (non-zero) detections in the rolling buffer
 *   \return	Median bin index of the valid detections, or 0 if fewer than ANALOG_MIN_VALID_DETECTIONS are present (debounce)
 *   \note	Zeros are "no target this frame" markers and MUST be excluded: like
 *   		the original mean, a brief pass occupies only a minority of the
 *   		~0.9s window, so including zeros would median to 0 and swallow it.
 *   		Among the detections a median still rejects the wild peak from a
 *   		single noise-corrupted frame, where a mean would be dragged by it.
 *   		Insertion sort on the collected samples; runs once per ~29ms frame.
 */
uint16_t analog_buffer_median(void) {
	const uint16_t n = ARRAY_SIZE(analog_last_peak_indexes);
	uint16_t tmp[ARRAY_SIZE(analog_last_peak_indexes)];
	uint16_t cnt = 0;

	/* Collect only valid detections (ignore the no-target zeros). */
	for (uint16_t i = 0; i < n; i++) {
		if (analog_last_peak_indexes[i] != 0) {
			tmp[cnt++] = analog_last_peak_indexes[i];
		}
	}

	/* Debounce: require a minimum number of valid detections before emitting a
	 * speed. Without this, a single false detection (the only non-zero sample
	 * among no-target zeros) would dominate the median and latch the output for
	 * the full ~0.9s window -- exactly how a transient harmonic off a vibrating
	 * reflector reads as a steady, wrong speed. */
	if (cnt < ANALOG_MIN_VALID_DETECTIONS) {
		return 0; /* no target, or an isolated transient spike: hold at zero */
	}

	for (uint16_t i = 1; i < cnt; i++) {
		uint16_t key = tmp[i];
		int j = (int) i - 1;
		while (j >= 0 && tmp[j] > key) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = key;
	}

	/* Median: middle sample (odd count) or mean of the two central (even). */
	if (cnt & 1u) {
		return tmp[cnt / 2];
	}
	return (uint16_t) (((uint32_t) tmp[cnt / 2 - 1] + (uint32_t) tmp[cnt / 2]) / 2);
}

/*! \fn     analog_compute_fft_on_cplted_sequence(BOOL remove_low_freqs)
 *   \brief  Compute FFT on completed adc samples
 *   \param	remove_low_freqs	Set to TRUE to remove low frequencies (~10km/h)
 *   \return	FFT peak, to be converted to kmh or mph
 *   \note	Hardened against ESP32 WiFi noise on the shared 5V rail:
 *   		(1) Hann window + DC removal before the FFT (kills leakage),
 *   		(2) SNR-based validity (rejects broadband bursts),
 *   		(3) rolling median + slew guard (rejects single bad frames),
 *   		(4) time-domain transient/clip rejection (drops corrupted frames).
 */
uint16_t analog_compute_fft_on_cplted_sequence(BOOL remove_low_freqs) {
	const uint16_t N = ARRAY_SIZE(analog_result_buffer1);
	float32_t max_value, floor_mean, mean, stddev;
	uint32_t max_index;

	/* --- (0) WiFi-TX blanking handshake + one-frame hangover -------------- *
	 * The ESP32 drives PA4 HIGH around its WiFi bursts (uploads / reconnects),
	 * which slump the shared rail and corrupt the radar reads. Drop those frames
	 * outright -- a direct, a-priori "contaminated" flag, far more reliable than
	 * inferring it from the noise statistics, and it skips the FFT so we also save
	 * CPU during the burst.
	 * The level is read here at frame-processing time, i.e. just AFTER the frame
	 * finished acquiring -- so a burst that ended partway through the frame (PA4
	 * already back LOW by now) would leak that partly-corrupted trailing frame.
	 * Guard it with a one-frame hangover: after any blanked frame, discard one
	 * more even if PA4 has dropped. (A short burst that both starts and ends
	 * within a single acquisition window still evades this level check; catching
	 * that needs a PA4 rising-edge latch (EXTI) or an ESP-side tail-hold on the
	 * line -- deferred, and backstopped by the SNR + median gates.) */
	static uint8_t blank_tail = 0;
	if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_SET) {
		blank_tail = 2;   /* discard this frame + one trailing (edge-corrupted) frame */
	}
	if (blank_tail != 0) {
		blank_tail--;
		analog_dbg_blanked = 1;
		uint16_t held = analog_buffer_median();
		return (uint16_t) ((float32_t) held * ANALOG_SAMPLE_RATE_HZ / N);
	}
	analog_dbg_blanked = 0;

	/* Overrun guard: if the ADC completed another DMA train before the main loop
	 * consumed the last frame (analog_overrun_flag, set in the ConvCplt ISR), the
	 * buffer we are about to read was mid-swap and may splice two frames. Discard
	 * it (hold the median) rather than run a torn frame through the FFT. */
	if (analog_overrun_flag != 0) {
		analog_overrun_flag = 0;
		analog_dbg_dropped = 1;
		uint16_t held = analog_buffer_median();
		return (uint16_t) ((float32_t) held * ANALOG_SAMPLE_RATE_HZ / N);
	}

	/* Convert to float */
	for (uint16_t i = 0; i < N; i++) {
		analog_temp_float_array[i] = (float32_t) analog_cplt_adc_dest_buf_pt[i];
	}

	/* --- (4) Time-domain transient / clip rejection ----------------------- *
	 * A WiFi TX burst on the shared 5V rail appears as an impulsive spike or
	 * rail clipping in the raw samples. If we see either, this frame's FFT is
	 * untrustworthy, so drop the whole frame: don't touch the rolling buffer,
	 * just report its current median so the output simply holds. */
	{
		float32_t sum = 0.0f, sumsq = 0.0f;
		for (uint16_t i = 0; i < N; i++) {
			float32_t s = analog_temp_float_array[i];
			sum += s;
			sumsq += s * s;
		}
		mean = sum / (float32_t) N;
		float32_t variance = (sumsq / (float32_t) N) - (mean * mean);
		stddev = (variance > 0.0f) ? sqrtf(variance) : 0.0f;
	}
	{
		uint16_t bad = 0;
		for (uint16_t i = 0; i < N; i++) {
			if (fabsf(analog_temp_float_array[i] - mean)
					> ANALOG_TRANSIENT_SIGMA * stddev) {
				bad++;
			}
		}
		analog_dbg_bad_count = bad;
		if (bad >= ANALOG_TRANSIENT_MIN_BAD) {
			analog_dbg_dropped = 1;
			uint16_t held = analog_buffer_median();
			return (uint16_t) ((float32_t) held * ANALOG_SAMPLE_RATE_HZ / N);
		}
		analog_dbg_dropped = 0;
	}

	/* --- (1) Remove DC and apply a Hann window before the FFT ------------- *
	 * Subtracting the mean removes the large mid-rail bias (and most of any
	 * mid-frame supply droop); the Hann window suppresses spectral leakage so
	 * transient / broadband energy no longer smears into a false peak.
	 * The window is generated on the fly with a cosine recurrence
	 * (a[i+1] = 2*cos(theta)*a[i] - a[i-1]) to avoid a 4KB lookup table on this
	 * 16KB-RAM part: one multiply per sample, no cosf in the hot loop. */
	{
		const float32_t theta = (2.0f * 3.14159265358979f) / (float32_t) (N - 1);
		const float32_t k = 2.0f * cosf(theta);
		float32_t a_im1 = cosf(theta); /* a[-1] = cos(theta) */
		float32_t a_i = 1.0f; /* a[0]  = cos(0) = 1 */
		/* Hann is symmetric: window[i] == window[N-1-i]. Run the cosine recurrence
		 * over only the first half and apply each value to BOTH ends. This halves
		 * the recurrence depth (so the float32 accumulation drift, up to ~2% at the
		 * far end of the old full-length loop, is roughly halved) and makes the
		 * window exactly symmetric, which keeps the FFT's real spectrum clean. N is
		 * even, so i and N-1-i never collide and every sample is written once. */
		for (uint16_t i = 0; i <= (N - 1) / 2; i++) {
			float32_t hann = 0.5f * (1.0f - a_i);
			analog_temp_float_array[i]         = (analog_temp_float_array[i] - mean) * hann;
			analog_temp_float_array[N - 1 - i] = (analog_temp_float_array[N - 1 - i] - mean) * hann;
			float32_t a_next = k * a_i - a_im1;
			a_im1 = a_i;
			a_i = a_next;
		}
	}

	/* RFFT transform */
	arm_rfft_fast_f32(&analog_fft, analog_temp_float_array, analog_rfft_output,
			0);

	/* Calculate magnitude of imaginary coefficients */
	arm_cmplx_mag_f32(analog_rfft_output, analog_temp_float_array, N / 2);

	/* Up to here takes 2.4ms in Release configuration on a 64MHz STM32F301 MCU */

	/* Peak search over the valid speed band: skip DC and (optionally) the
	 * lowest bins where supply-droop residue lives. remove_low_freqs raises
	 * the floor to ~10 km/h. This replaces the old explicit bin zeroing. */
	uint16_t min_bin = (remove_low_freqs != FALSE) ? 10 : ANALOG_MIN_BIN;
	uint16_t band = (N / 2) - min_bin;
	arm_max_f32(&analog_temp_float_array[min_bin], band, &max_value, &max_index);
	max_index += min_bin;

	/* --- (2) SNR-based validity ------------------------------------------ *
	 * Compare the peak against the mean magnitude of the band (the noise
	 * floor). A broadband WiFi burst lifts the whole floor, so its peak/floor
	 * ratio stays low and the frame is rejected; a real Doppler tone towers
	 * over the floor. This replaces the old absolute "< 100000" threshold,
	 * which a broadband burst could exceed while carrying no real signal. */
	{
		float32_t fsum = 0.0f;
		for (uint16_t i = min_bin; i < (N / 2); i++) {
			fsum += analog_temp_float_array[i];
		}
		/* Exclude the peak's own Hann main lobe (max_index +/- 2 bins, clamped to
		 * the band) from the noise-floor estimate. Leaving it in inflates the floor
		 * for a strong tone, which silently tightens the SNR gate (~7%) and caps the
		 * reported snr; removing it makes ANALOG_MIN_SNR exact and lets snr scale
		 * linearly for strong targets. */
		uint32_t lobe_lo = (max_index > (uint32_t) min_bin + 2) ? (max_index - 2) : min_bin;
		uint32_t lobe_hi = (max_index + 2 < (uint32_t) (N / 2)) ? (max_index + 2) : (uint32_t) (N / 2 - 1);
		uint16_t excluded = 0;
		for (uint32_t i = lobe_lo; i <= lobe_hi; i++) {
			fsum -= analog_temp_float_array[i];
			excluded++;
		}
		uint16_t denom = (band > excluded) ? (uint16_t) (band - excluded) : 1;
		floor_mean = fsum / (float32_t) denom;
	}
	BOOL valid_detection = (floor_mean > 0.0f)
			&& (max_value >= ANALOG_MIN_SNR * floor_mean);

	/* Capture for the 'd' diagnostic dump. */
	analog_dbg_peak_bin = (uint16_t) max_index;
	analog_dbg_peak_mag = (uint32_t) max_value;
	analog_dbg_floor_mean = (uint32_t) floor_mean;

	/* --- (3) Rolling median ---------------------------------------------- *
	 * This frame's measurement is just the peak bin (0 if it failed the SNR
	 * gate). It is pushed into the circular buffer and the output is the median
	 * of the valid entries -- which on its own rejects isolated bad frames.
	 * NOTE: do NOT add a slew/jump guard here. A real target appearing is a
	 * large jump away from the noise-established median, so a jump guard would
	 * reject every real signal and pin the output to the noise floor. */
	uint16_t measurement = (valid_detection != FALSE) ? (uint16_t) max_index : 0;

	/* Store into the circular buffer */
	analog_last_peak_indexes[analog_cur_peak_fill_idx++] = measurement;
	if (analog_cur_peak_fill_idx == ARRAY_SIZE(analog_last_peak_indexes)) {
		analog_cur_peak_fill_idx = 0;
	}

	/* Output speed = median bin -> Doppler frequency (Hz). */
	uint16_t med = analog_buffer_median();
	return (uint16_t) ((float32_t) med * ANALOG_SAMPLE_RATE_HZ / N);
}

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
	/* USER CODE BEGIN Error_Handler_Debug */
	/* A failed HAL init must not silently run with half-initialised peripherals.
	 * Stop and let the independent watchdog reset the MCU so a transient boot
	 * fault (e.g. a brownout during init) self-recovers. If we reached here BEFORE
	 * main() started the IWDG, start it now -- otherwise an early-init failure
	 * would hang forever with no watchdog to reset it (a silent brick). Starting
	 * an already-running IWDG is harmless; we then spin without refreshing it, so
	 * the ~2s timeout resets us wherever the failure occurred. A persistent fault
	 * becomes an observable ~2s reset loop (the ESP32 sees the STM link stay dead
	 * and keeps WiFi up for an OTA reflash) rather than a silent halt. */
	IWDG->KR  = 0x5555;   /* unlock PR/RLR */
	IWDG->PR  = 0x04;     /* prescaler /64 */
	IWDG->RLR = 1250;     /* reload -> ~2.0s */
	IWDG->KR  = 0xCCCC;   /* start (no-op if already running) */
	__disable_irq();
	while (1) {
	}
	/* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
