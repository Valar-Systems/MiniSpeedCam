"""
Inject architecture + specs flags into the link stage.

PlatformIO routes most of `build_flags` to the compiler only. For a hard-float
Cortex-M4 image, the FPU/float-ABI and the C-library specs MUST be identical at
link time, otherwise the linker either fails ("uses VFP register arguments,
output does not") or pulls in the wrong (soft-float) libc/libm. This script
mirrors those flags onto LINKFLAGS so compile and link stay in lockstep.
"""
Import("env")

env.Append(
    LINKFLAGS=[
        "-mcpu=cortex-m4",
        "-mthumb",
        "-mfpu=fpv4-sp-d16",
        "-mfloat-abi=hard",
        "--specs=nano.specs",
        "--specs=nosys.specs",
        "-Wl,--gc-sections",
    ]
)
