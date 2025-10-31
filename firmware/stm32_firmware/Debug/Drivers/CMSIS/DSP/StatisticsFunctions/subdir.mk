################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.c 

OBJS += \
./Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.o 

C_DEPS += \
./Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/CMSIS/DSP/StatisticsFunctions/%.o Drivers/CMSIS/DSP/StatisticsFunctions/%.su Drivers/CMSIS/DSP/StatisticsFunctions/%.cyclo: ../Drivers/CMSIS/DSP/StatisticsFunctions/%.c Drivers/CMSIS/DSP/StatisticsFunctions/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DARM_MATH_CM4 -DARM_DSP_CONFIG_TABLES -DARM_FAST_ALLOW_TABLES -DARM_FFT_ALLOW_TABLES -DARM_TABLE_TWIDDLECOEF_F32_512 -DARM_TABLE_BITREVIDX_FLT_512 -DARM_TABLE_TWIDDLECOEF_RFFT_F32_1024 -DARM_ALL_FAST_TABLES -DARM_MATH_LOOPUNROLL -DDISABLEFLOAT16 -DUSE_HAL_DRIVER -DSTM32F301x8 -c -I../Core/Inc -I../Drivers/CMSIS/DSP/Include -I../Drivers/STM32F3xx_HAL_Driver/Inc -I../Drivers/STM32F3xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F3xx/Include -I../Drivers/CMSIS/Include -I../FATFS/Target -I../FATFS/App -I../Middlewares/Third_Party/FatFs/src -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-CMSIS-2f-DSP-2f-StatisticsFunctions

clean-Drivers-2f-CMSIS-2f-DSP-2f-StatisticsFunctions:
	-$(RM) ./Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.cyclo ./Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.d ./Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.o ./Drivers/CMSIS/DSP/StatisticsFunctions/arm_max_f32.su

.PHONY: clean-Drivers-2f-CMSIS-2f-DSP-2f-StatisticsFunctions

