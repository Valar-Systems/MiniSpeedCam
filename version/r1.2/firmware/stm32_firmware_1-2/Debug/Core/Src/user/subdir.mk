################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/user/analog.c \
../Core/Src/user/clocks.c \
../Core/Src/user/debug.c \
../Core/Src/user/display.c \
../Core/Src/user/expander_board.c \
../Core/Src/user/main.c 

OBJS += \
./Core/Src/user/analog.o \
./Core/Src/user/clocks.o \
./Core/Src/user/debug.o \
./Core/Src/user/display.o \
./Core/Src/user/expander_board.o \
./Core/Src/user/main.o 

C_DEPS += \
./Core/Src/user/analog.d \
./Core/Src/user/clocks.d \
./Core/Src/user/debug.d \
./Core/Src/user/display.d \
./Core/Src/user/expander_board.d \
./Core/Src/user/main.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Src/user/%.o Core/Src/user/%.su Core/Src/user/%.cyclo: ../Core/Src/user/%.c Core/Src/user/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DARM_MATH_CM4 -DARM_DSP_CONFIG_TABLES -DARM_FAST_ALLOW_TABLES -DARM_FFT_ALLOW_TABLES -DARM_TABLE_TWIDDLECOEF_F32_512 -DARM_TABLE_BITREVIDX_FLT_512 -DARM_TABLE_TWIDDLECOEF_RFFT_F32_1024 -DARM_ALL_FAST_TABLES -DARM_MATH_LOOPUNROLL -DDISABLEFLOAT16 -DUSE_HAL_DRIVER -DSTM32F301x8 -c -I../Core/Inc -I../Drivers/CMSIS/DSP/Include -I../Drivers/STM32F3xx_HAL_Driver/Inc -I../Drivers/STM32F3xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F3xx/Include -I../Drivers/CMSIS/Include -I../FATFS/Target -I../FATFS/App -I../Middlewares/Third_Party/FatFs/src -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src-2f-user

clean-Core-2f-Src-2f-user:
	-$(RM) ./Core/Src/user/analog.cyclo ./Core/Src/user/analog.d ./Core/Src/user/analog.o ./Core/Src/user/analog.su ./Core/Src/user/clocks.cyclo ./Core/Src/user/clocks.d ./Core/Src/user/clocks.o ./Core/Src/user/clocks.su ./Core/Src/user/debug.cyclo ./Core/Src/user/debug.d ./Core/Src/user/debug.o ./Core/Src/user/debug.su ./Core/Src/user/display.cyclo ./Core/Src/user/display.d ./Core/Src/user/display.o ./Core/Src/user/display.su ./Core/Src/user/expander_board.cyclo ./Core/Src/user/expander_board.d ./Core/Src/user/expander_board.o ./Core/Src/user/expander_board.su ./Core/Src/user/main.cyclo ./Core/Src/user/main.d ./Core/Src/user/main.o ./Core/Src/user/main.su

.PHONY: clean-Core-2f-Src-2f-user

