################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include Middlewares/Third_Party/FatFs/src/option/subdir.mk
-include Middlewares/Third_Party/FatFs/src/subdir.mk
-include FATFS/Target/subdir.mk
-include FATFS/App/subdir.mk
-include Drivers/STM32F3xx_HAL_Driver/Src/subdir.mk
-include Drivers/CMSIS/DSP/TransformFunctions/subdir.mk
-include Drivers/CMSIS/DSP/StatisticsFunctions/subdir.mk
-include Drivers/CMSIS/DSP/ComplexMathFunctions/subdir.mk
-include Drivers/CMSIS/DSP/CommonTables/subdir.mk
-include Core/Startup/subdir.mk
-include Core/Src/subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := MiniSpeedCam
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 
EXECUTABLES += \
MiniSpeedCam.elf \

MAP_FILES += \
MiniSpeedCam.map \

SIZE_OUTPUT += \
default.size.stdout \

OBJDUMP_LIST += \
MiniSpeedCam.list \

OBJCOPY_HEX += \
MiniSpeedCam.hex \

OBJCOPY_BIN += \
MiniSpeedCam.bin \


# All Target
all: main-build

# Main-build Target
main-build: MiniSpeedCam.elf secondary-outputs

# Tool invocations
MiniSpeedCam.elf MiniSpeedCam.map: $(OBJS) $(USER_OBJS) C:\Github\MiniSpeedCam\firmware\stm32_firmware\STM32F301K8UX_FLASH.ld makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-gcc -o "MiniSpeedCam.elf" @"objects.list" $(USER_OBJS) $(LIBS) -mcpu=cortex-m4 -T"C:\Github\MiniSpeedCam\firmware\stm32_firmware\STM32F301K8UX_FLASH.ld" --specs=nosys.specs -Wl,-Map="MiniSpeedCam.map" -Wl,--gc-sections -static -L../Middlewares/ST/ARM/DSP/Lib --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -Wl,--start-group -lc -lm -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

default.size.stdout: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-size  $(EXECUTABLES)
	@echo 'Finished building: $@'
	@echo ' '

MiniSpeedCam.list: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objdump -h -S $(EXECUTABLES) > "MiniSpeedCam.list"
	@echo 'Finished building: $@'
	@echo ' '

MiniSpeedCam.hex: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objcopy  -O ihex $(EXECUTABLES) "MiniSpeedCam.hex"
	@echo 'Finished building: $@'
	@echo ' '

MiniSpeedCam.bin: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objcopy  -O binary $(EXECUTABLES) "MiniSpeedCam.bin"
	@echo 'Finished building: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) MiniSpeedCam.bin MiniSpeedCam.elf MiniSpeedCam.hex MiniSpeedCam.list MiniSpeedCam.map default.size.stdout
	-@echo ' '

secondary-outputs: $(SIZE_OUTPUT) $(OBJDUMP_LIST) $(OBJCOPY_HEX) $(OBJCOPY_BIN)

fail-specified-linker-script-missing:
	@echo 'Error: Cannot find the specified linker script. Check the linker settings in the build configuration.'
	@exit 2

warn-no-linker-script-specified:
	@echo 'Warning: No linker script specified. Check the linker settings in the build configuration.'

.PHONY: all clean dependents main-build fail-specified-linker-script-missing warn-no-linker-script-specified

-include ../makefile.targets
