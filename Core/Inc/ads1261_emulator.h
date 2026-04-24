#ifndef ADS1261_EMULATOR_H
#define ADS1261_EMULATOR_H

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define ADS1261_EMU_REG_COUNT 19u  // 0x00 to 0x12

typedef enum {
    EMU_STATE_IDLE,
    EMU_STATE_RREG_COUNT,
    EMU_STATE_RREG_DATA,
    EMU_STATE_WREG_COUNT,
    EMU_STATE_WREG_DATA,
    EMU_STATE_RDATA_B1,
    EMU_STATE_RDATA_B2,
    EMU_STATE_RDATA_B3,
} ADS1261_EmuState_t;

typedef struct {
    SPI_HandleTypeDef   *hspi;
    uint8_t              regs[ADS1261_EMU_REG_COUNT];
    int32_t              adc_value;         // Valeur 24-bit simulée
    bool                 conversion_running;
    bool                 strict_mode;
    ADS1261_EmuState_t   state;
    uint8_t              reg_addr;
    uint8_t              remaining;
    uint16_t             drdy_delay_ticks;
    uint16_t             drdy_countdown;
} ADS1261_Emulator_t;

enum {
    ADS1261_EMU_TRACE_WARN_UNSUPPORTED_FEATURE = 1u << 0,
    ADS1261_EMU_TRACE_WARN_DRDY_NOT_READY = 1u << 1,
};

typedef struct {
    uint32_t seq;
    uint8_t rx;
    uint8_t tx;
    uint8_t state_before;
    uint8_t state_after;
    uint8_t warn_flags;
    uint8_t aux;
} ADS1261_Emulator_Trace_t;

void ADS1261_Emulator_Init(ADS1261_Emulator_t *emu, SPI_HandleTypeDef *hspi);
void ADS1261_Emulator_SetADCValue(ADS1261_Emulator_t *emu, int32_t value);
void ADS1261_Emulator_SetStrictMode(ADS1261_Emulator_t *emu, bool enable);
void ADS1261_Emulator_SetDRDYDelayTicks(ADS1261_Emulator_t *emu, uint16_t ticks);
void ADS1261_Emulator_Tick(ADS1261_Emulator_t *emu);
void ADS1261_Emulator_SPI_IRQHandler(ADS1261_Emulator_t *emu);
bool ADS1261_Emulator_PopTrace(ADS1261_Emulator_t *emu, ADS1261_Emulator_Trace_t *out);

#endif // ADS1261_EMULATOR_H