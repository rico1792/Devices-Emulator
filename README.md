# Device Emulator

A firmware-level emulator for SPI and I2C peripheral devices, running on an **STM32F446 (NUCLEO-F446RE)**. The emulator acts as a drop-in replacement for real hardware devices, allowing host-side drivers and software stacks to be validated without physical chips on the bench.

## Purpose

This project is designed for use in **PIO (Production/Integration) validation test setups**. Instead of wiring up real sensors or ADCs, the STM32 board impersonates the target device over SPI or I2C, responding to commands and returning configurable data. This makes it easy to:

- Test driver logic in isolation
- Inject specific ADC values, error conditions, or edge cases
- Automate regression testing without hardware variability

## Supported Devices

| Device | Interface | Status |
|--------|-----------|--------|
| [ADS1261](https://www.ti.com/product/ADS1261) | SPI | ✅ Implemented |

> Additional device emulators will be added over time.

## ADS1261 Emulator

The ADS1261 is a 24-bit, high-precision ADC from Texas Instruments. The emulator supports:

- Full register map (19 registers, `0x00`–`0x12`)
- Commands: `RESET`, `START`, `STOP`, `RDATA`, `RREG`, `WREG`
- Simulated DRDY (data-ready) signal with configurable delay
- Configurable 24-bit ADC output value
- Strict mode for stricter protocol validation
- SPI transaction trace buffer for debug/inspection

## Hardware

| Item | Details |
|------|---------|
| Board | NUCLEO-F446RE |
| MCU | STM32F446RE (ARM Cortex-M4, 180 MHz) |
| Toolchain | GCC ARM None EABI |
| Build system | CMake + Ninja |
| Cube version | STM32CubeMX (HAL) |

## Building

Prerequisites: `arm-none-eabi-gcc`, `cmake`, `ninja`.

```bash
cmake --preset Debug
cmake --build --preset Debug
```

The output ELF is generated at `build/Debug/Device-emulator.elf`.

## Flashing

Using STM32CubeProgrammer CLI with an ST-Link probe:

```bash
STM32_Programmer_CLI.exe -c port=swd -d build/Debug/Device-emulator.elf -v -rst
```

## VS Code Tasks

Two tasks are available in the VS Code task runner (`Terminal > Run Task`):

| Task | Description |
|------|-------------|
| **Build Debug (CMake Preset)** | Configures and builds the project using the `Debug` CMake preset. |
| **Flash STM32F446 (STLink)** | Flashes the built ELF to any connected NUCLEO board via ST-Link (SWD). Requires STM32CubeCLT installed at `C:/ST/STM32CubeCLT_1.19.0/`. |

> The flash task connects to whichever ST-Link probe is detected automatically. If multiple probes are connected, add `-c port=swd sn=<serial>` to target a specific one.

## Project Structure

```
Core/
  Inc/          # Headers (emulator interfaces, HAL config)
  Src/          # Application source (emulator logic, main, peripherals)
Drivers/        # STM32 HAL + CMSIS (generated)
cmake/          # Toolchain files
CMakeLists.txt
CMakePresets.json
startup_stm32f446xx.s
STM32F446XX_FLASH.ld
```

## License

This project uses STM32 HAL drivers provided by STMicroelectronics under their standard license. See `Drivers/` subdirectories for individual license files.
