#ifndef EXPANDER_H_
#define EXPANDER_H_

#include "stm32f3xx_hal.h"
#include "user/defines.h"

/* Defines */
#define SD_CD_Pin 			GPIO_PIN_0
#define SD_CD_Pin_Port 		GPIOA
#define SWITCH_Pin 			GPIO_PIN_1
#define SWITCH_GPIO_Port 	GPIOA
#define SPI_SS_Pin 			GPIO_PIN_15
#define SPI_SS_GPIO_Port 	GPIOA
#define SD_CS_Pin 			SPI_SS_Pin
#define SD_CS_GPIO_Port 	SPI_SS_GPIO_Port

/* Prototypes */
BOOL expander_is_kph_selected(void);
BOOL expander_init(void);

#endif
