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
#include "user/defines.h"
#include "user/ascii_lib.h"
#include "arm_math.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

// Do we skip data analysis after updating display?
//#define SKIP_ADC_DATA_AFTER_DISPLAY
// After how many FFTs do we update display
#define FFT_CNT_DISP 10 // 10 original

#define DOPPLER_Pin GPIO_PIN_0
#define DOPPLER_GPIO_Port GPIOB

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

/* Boolean set when a DMA TX transfer is in progress */
volatile BOOL debug_uart_tx_in_progress = FALSE;
// Define to enable current transient test
//#define CUR_TRANSIENT_TEST

volatile uint16_t analog_result_buffer1[1024];
volatile uint16_t analog_result_buffer2[1024];
volatile uint16_t *analog_cur_adc_dest_buf_pt;
volatile uint16_t *analog_cplt_adc_dest_buf_pt;
volatile BOOL analog_fdma_transfer_started;
volatile BOOL analog_adc_train_done_flag;
arm_rfft_fast_instance_f32 analog_fft;
float32_t analog_temp_float_array[1024];
float32_t analog_rfft_output[1024];
uint16_t analog_last_peak_indexes[32];
uint16_t analog_cur_peak_fill_idx = 0;

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

// Analog
static uint16_t analog_compute_fft_on_cplted_sequence(BOOL remove_low_freqs);
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
	uint16_t fft_nb_counter = 0;

#ifdef CUR_TRANSIENT_TEST
  	uint16_t temp_counter = 0;
  #endif

	printf("Connection Successful! \r\n");

	printf("CDM324-V2 fw v%d.%d, compiled %s %s\r\n", FW_MAJOR, FW_MINOR,
	__DATE__, __TIME__);

	/* Analog input init */
	analog_init();

	/* Trigger analog conversions */
	analog_trigger_conversion();

	/* USER CODE END 2 */

	/* Infinite loop */
	/* USER CODE BEGIN WHILE */
	while (1) {

		/* Sequence of ADC measurements complete? */
		if (analog_get_and_clear_adc_measurement_done() != FALSE) {
			/* Every few ms we display the speed and depending on define not do anything with the data has the current draw is enough to have an impact on the +5V PSU */
			if (fft_nb_counter++ == FFT_CNT_DISP) {
				/* Reset counter */
				fft_nb_counter = 0;
			}

			/* Should we display the result? */
			if (fft_nb_counter == FFT_CNT_DISP) {
				/* Valid return? */
				if (last_fft_return == 0) {
					/* Pull ESP32 line LOW */
					HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);

				} else {
					/* Pull ESP32 line HIGH */
					HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);

				}
			}

			/* Debug ADC output buffer ? */
			if (output_debug_enabled != FALSE) {
				analog_output_conversion_buffer_to_uart();
			}

			/* Compute FFT */
			last_fft_return = analog_compute_fft_on_cplted_sequence(
					remove_low_freqs);

			/* Debug FFT output buffer ? */
			if (output_debug_enabled != FALSE) {
				analog_output_current_fft_to_uart(150);
			}

			/* Commands from UART */
			char uart_input = debug_get_char_from_uart();
			if (uart_input == 'a') {
				output_debug_enabled = TRUE;
			} else if (uart_input == 's') {
				output_debug_enabled = FALSE;
			} else if (uart_input == 'h') {
				remove_low_freqs = TRUE;
			} else if (uart_input == 'l') {
				remove_low_freqs = FALSE;
			} else if (uart_input == 'k') {
				printf("%d\r\n", (uint16_t) (last_fft_return * 0.2262295));
			} else if (uart_input == 'm') {
				printf("%d\r\n", (uint16_t) (last_fft_return * 0.1449275));
			}
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
 *   \brief  Send buffer over UART
 *   \param	buffer	pointer to the buffer
 *   \param	size	buffer size
 *   \note	Please note that this is done using DMA and isn't blocking!
 */
void debug_dma_output_buffer(uint8_t *buffer, uint16_t size) {
	while (debug_uart_tx_in_progress != FALSE)
		;
	debug_uart_tx_in_progress = TRUE;
	HAL_UART_Transmit_DMA(&huart1, buffer, size);
	//HAL_UART_Transmit_DMA(&huart2, buffer, size);
}

/*! \fn     debug_get_char_from_uart(void)
 *   \brief  Get debug char from UART
 *   \return	0 if nothing was received, otherwise the char
 */
char debug_get_char_from_uart(void) {
	char temp_char = 0;

	if ((__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) ? SET : RESET) == SET) {
		HAL_UART_Receive(&huart1, (uint8_t*) &temp_char, sizeof(temp_char), 1);
		return temp_char;
	} else {
		return 0;
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

/*! \fn     analog_compute_fft_on_cplted_sequence(BOOL remove_low_freqs)
 *   \brief  Compute FFT on completed adc samples
 *   \param	remove_low_freqs	Set to TRUE to remove low frequencies (~10km/h)
 *   \return	FFT peak, to be converted to kmh or mph
 */
uint16_t analog_compute_fft_on_cplted_sequence(BOOL remove_low_freqs) {
	float32_t max_value;
	uint32_t max_index;

	/* Convert to float */
	for (uint16_t i = 0; i < ARRAY_SIZE(analog_result_buffer1); i++) {
		analog_temp_float_array[i] = (float) analog_cplt_adc_dest_buf_pt[i];
	}

	/* RFFT transform */
	arm_rfft_fast_f32(&analog_fft, analog_temp_float_array, analog_rfft_output,
			0);

	/* Calculate magnitude of imaginary coefficients */
	arm_cmplx_mag_f32(analog_rfft_output, analog_temp_float_array,
	ARRAY_SIZE(analog_result_buffer1) / 2);

	/* Up to here takes 2.4ms in Release configuration on a 64MHz STM32F301 MCU */

	/* Remove low freqs ? */
	if (remove_low_freqs != FALSE) {
		memset(analog_temp_float_array, 0,
				sizeof(analog_temp_float_array[0]) * 10);
	}

	/* Set DC component to 0 */
	analog_temp_float_array[0] = 0;

	/* Extract peak frequency */
	arm_max_f32(analog_temp_float_array, ARRAY_SIZE(analog_result_buffer1) / 2,
			&max_value, &max_index);

	/* Fill current peak index if peak is valid */
	if (max_value < 100000) {
		analog_last_peak_indexes[analog_cur_peak_fill_idx++] = 0;
	} else {
		analog_last_peak_indexes[analog_cur_peak_fill_idx++] = max_index;
	}

	/* Handle buffer wrapover */
	if (analog_cur_peak_fill_idx == ARRAY_SIZE(analog_last_peak_indexes)) {
		analog_cur_peak_fill_idx = 0;
	}

	/* Compute average speed over buffer */
	float32_t cumul = 0;
	uint16_t valid_samples = 0;
	for (uint16_t i = 0; i < ARRAY_SIZE(analog_last_peak_indexes); i++) {
		if (analog_last_peak_indexes[i] != 0) {
			cumul += (float32_t) analog_last_peak_indexes[i];
			valid_samples++;
		}
	}

	/* Return averaged speed from this */
	float32_t max_freq = 0;
	if (valid_samples != 0) {
		max_freq = cumul / valid_samples;
	}
	max_freq = max_freq * 35714 / ARRAY_SIZE(analog_result_buffer1);
	return (uint16_t) max_freq;
}

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void) {
	/* USER CODE BEGIN Error_Handler_Debug */
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
