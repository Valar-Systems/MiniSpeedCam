# MiniSpeedCam STM32 firmware — PlatformIO port

This is a [PlatformIO](https://platformio.org/) build of the MiniSpeedCam
firmware for the **STM32F301K8U6** (Cortex-M4F, 64 KB Flash / 16 KB RAM),
ported from the original STM32CubeIDE project in [`../stm32_firmware`](../stm32_firmware).

It is functionally identical: the same application code, ST HAL drivers,
CMSIS-DSP, FatFs middleware, startup file and linker script are used. Only the
build system changed.

## Why a framework-less build

The project is configured **without** PlatformIO's `stm32cube` framework. Instead
it vendors the exact sources from the CubeIDE project. This guarantees the build
uses the same known-good HAL/CMSIS/DSP versions as the original — no risk of a
framework version mismatch or duplicate-symbol conflicts. PlatformIO only
provides the GNU Arm toolchain and orchestrates the build.

## Usage

```sh
pio run            # build  -> .pio/build/genericSTM32F301K8/firmware.{elf,bin,hex}
pio run -t upload  # flash via ST-Link (default)
pio run -t clean   # clean
pio run -t size    # memory usage
```

First run downloads the `toolchain-gccarmnoneeabi` package automatically.

## Layout

| Path | Origin |
|------|--------|
| `Core/`         | Application + CubeMX-generated code (`main.c`, IT handlers, MSP, syscalls, startup) |
| `Drivers/`      | ST STM32F3xx HAL driver + CMSIS core/device + CMSIS-DSP |
| `FATFS/`, `Middlewares/` | FatFs glue layer and middleware |
| `STM32F301K8UX_FLASH.ld` | Original CubeIDE linker script (64 KB Flash / 16 KB RAM) |
| `boards/genericSTM32F301K8.json` | Custom board definition (the F301K8 is not a built-in PlatformIO board) |
| `link_flags.py`  | Mirrors the FPU / float-ABI / libc-specs flags onto the link stage |
| `platformio.ini` | Build configuration (flags & defines match the CubeIDE project verbatim) |

## Build configuration notes

- **MCU defines:** `USE_HAL_DRIVER`, `STM32F301x8`
- **CMSIS-DSP:** `ARM_MATH_CM4` plus the same `ARM_*_TABLES` config defines as the
  original project, so only the FFT/twiddle tables actually used are linked in.
- **ABI:** `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb` (hard float),
  `--specs=nano.specs --specs=nosys.specs`, `-Os`.

The original project (CubeIDE) remains untouched in `../stm32_firmware`; both can
coexist.

## Memory footprint (-Os)

```
Flash: ~44.6 KB / 64 KB  (~68%)
RAM:   ~15.7 KB / 16 KB  (~96%, incl. 0x200 heap + 0x400 stack reservation)
```
