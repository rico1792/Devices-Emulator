#include "ads1261_emulator.h"

#include <string.h>

// Commands
#define CMD_RESET 0x06
#define CMD_START 0x08
#define CMD_STOP 0x0A
#define CMD_RDATA 0x12
#define CMD_RREG 0x20
#define CMD_WREG 0x40

// Registers
#define REG_ID 0x00
#define REG_STATUS 0x01
#define REG_MODE3 0x05

// STATUS bits
#define STATUS_DRDY 0x04
#define STATUS_RESET 0x01

// DEV_ID=0x07 dans bits[7:4], REV_ID=0x00 dans bits[3:0]
#define ADS1261_ID_VALUE 0x70

// MODE3 bits [1:0] are used for STATUS/CRC framing on real devices.
// The emulator currently does not extend data frames accordingly.
#define MODE3_STATUS_CRC_MASK 0x03u

#define EMU_TRACE_BUF_SIZE 128u

static volatile ADS1261_Emulator_Trace_t g_trace_buf[EMU_TRACE_BUF_SIZE];
static volatile uint16_t g_trace_wr = 0u;
static volatile uint16_t g_trace_rd = 0u;
static volatile uint32_t g_trace_seq = 0u;
static volatile uint8_t g_last_tx = 0u;

static inline void trace_push(uint8_t rx, uint8_t tx, uint8_t state_before,
                              uint8_t state_after, uint8_t warn_flags,
                              uint8_t aux) {
  uint16_t next = (uint16_t)((g_trace_wr + 1u) % EMU_TRACE_BUF_SIZE);

  // Drop oldest event on overflow to keep most recent traffic.
  if (next == g_trace_rd) {
    g_trace_rd = (uint16_t)((g_trace_rd + 1u) % EMU_TRACE_BUF_SIZE);
  }

  g_trace_buf[g_trace_wr].seq = ++g_trace_seq;
  g_trace_buf[g_trace_wr].rx = rx;
  g_trace_buf[g_trace_wr].tx = tx;
  g_trace_buf[g_trace_wr].state_before = state_before;
  g_trace_buf[g_trace_wr].state_after = state_after;
  g_trace_buf[g_trace_wr].warn_flags = warn_flags;
  g_trace_buf[g_trace_wr].aux = aux;
  g_trace_wr = next;
}

static const uint8_t default_regs[ADS1261_EMU_REG_COUNT] = {
    ADS1261_ID_VALUE,  // REG_ID    (0x00)
    0x01,              // REG_STATUS (0x01) - RESET bit après reset
    0x24,              // REG_MODE0  (0x02)
    0x01,              // REG_MODE1  (0x03)
    0x00,              // REG_MODE2  (0x04)
    0x00,              // REG_MODE3  (0x05)
    0x05,              // REG_REF    (0x06)
    0x00,              // REG_OFCAL0 (0x07)
    0x00,              // REG_OFCAL1 (0x08)
    0x00,              // REG_OFCAL2 (0x09)
    0x00,              // REG_FSCAL0 (0x0A)
    0x00,              // REG_FSCAL1 (0x0B)
    0x40,              // REG_FSCAL2 (0x0C)
    0xFF,              // REG_IMUX   (0x0D)
    0x00,              // REG_IMAG   (0x0E)
    0x00,              // REG_RESERVED (0x0F)
    0x00,              // REG_PGA    (0x10)
    0xFF,              // REG_INPMUX (0x11)
    0x00,              // REG_INPBIAS (0x12)
};

static void reset_state(ADS1261_Emulator_t *emu) {
  memcpy(emu->regs, default_regs, ADS1261_EMU_REG_COUNT);
  emu->conversion_running = false;
  emu->state = EMU_STATE_IDLE;
  emu->drdy_countdown = 0u;
}

static inline void spi_put(ADS1261_Emulator_t *emu, uint8_t byte) {
  emu->hspi->Instance->DR = byte;
  g_last_tx = byte;
}

void ADS1261_Emulator_Init(ADS1261_Emulator_t *emu, SPI_HandleTypeDef *hspi) {
  emu->hspi = hspi;
  emu->adc_value = 0;
  emu->strict_mode = false;
  emu->drdy_delay_ticks = 0u;
  emu->drdy_countdown = 0u;
  reset_state(emu);
  __disable_irq();
  g_trace_wr = 0u;
  g_trace_rd = 0u;
  g_trace_seq = 0u;
  g_last_tx = 0u;
  __enable_irq();

  // Pré-charger 0x00 dans le TX buffer
  spi_put(emu, 0x00);

  // Activer les interruptions RXNE et OVR
  __HAL_SPI_ENABLE_IT(hspi, SPI_IT_RXNE | SPI_IT_ERR);
  __HAL_SPI_ENABLE(hspi);
}

void ADS1261_Emulator_SetADCValue(ADS1261_Emulator_t *emu, int32_t value) {
  // Masque 24 bits, interprétation signée
  emu->adc_value = value & 0x00FFFFFF;
}

void ADS1261_Emulator_SetStrictMode(ADS1261_Emulator_t *emu, bool enable) {
  emu->strict_mode = enable;
}

void ADS1261_Emulator_SetDRDYDelayTicks(ADS1261_Emulator_t *emu,
                                        uint16_t ticks) {
  emu->drdy_delay_ticks = ticks;
  emu->drdy_countdown = 0u;
  if (ticks > 0u) {
    emu->regs[REG_STATUS] &= (uint8_t)~STATUS_DRDY;
  }
}

void ADS1261_Emulator_Tick(ADS1261_Emulator_t *emu) {
  if (!emu->conversion_running) {
    return;
  }
  if (emu->drdy_countdown > 0u) {
    emu->drdy_countdown--;
    if (emu->drdy_countdown == 0u) {
      emu->regs[REG_STATUS] |= STATUS_DRDY;
    }
  }
}

void ADS1261_Emulator_SPI_IRQHandler(ADS1261_Emulator_t *emu) {
  SPI_TypeDef *spi = emu->hspi->Instance;
  uint32_t sr = spi->SR;
  uint8_t state_before = (uint8_t)emu->state;
  uint8_t warn_flags = 0u;
  uint8_t aux = 0u;

  // Effacer l'overrun si présent
  if (sr & SPI_SR_OVR) {
    volatile uint32_t tmp = spi->DR;
    tmp = spi->SR;
    (void)tmp;
  }

  if (!(sr & SPI_SR_RXNE)) {
    return;
  }

  uint8_t rx = (uint8_t)spi->DR;

  switch (emu->state) {
    case EMU_STATE_IDLE:
      if (rx == CMD_RESET) {
        reset_state(emu);
        spi_put(emu, 0x00);

      } else if (rx == CMD_START) {
        emu->conversion_running = true;
        if (emu->drdy_delay_ticks == 0u) {
          emu->regs[REG_STATUS] |= STATUS_DRDY;
          emu->drdy_countdown = 0u;
        } else {
          emu->regs[REG_STATUS] &= (uint8_t)~STATUS_DRDY;
          emu->drdy_countdown = emu->drdy_delay_ticks;
        }
        spi_put(emu, 0x00);

      } else if (rx == CMD_STOP) {
        emu->conversion_running = false;
        emu->regs[REG_STATUS] &= ~STATUS_DRDY;
        spi_put(emu, 0x00);

      } else if (rx == CMD_RDATA) {
        if ((emu->regs[REG_STATUS] & STATUS_DRDY) == 0u) {
          if (emu->strict_mode) {
            warn_flags |= ADS1261_EMU_TRACE_WARN_DRDY_NOT_READY;
          }
          spi_put(emu, 0x00);
        } else {
          uint8_t msb = (uint8_t)((emu->adc_value >> 16) & 0xFF);
          spi_put(emu, msb);  // Sera envoyé pendant l'octet suivant
          emu->state = EMU_STATE_RDATA_B1;
        }

      } else if ((rx & 0xE0) == CMD_RREG) {
        emu->reg_addr = rx & 0x1F;
        if (emu->reg_addr >= ADS1261_EMU_REG_COUNT) {
          emu->reg_addr = ADS1261_EMU_REG_COUNT - 1u;
        }
        spi_put(emu, 0x00);
        emu->state = EMU_STATE_RREG_COUNT;

      } else if ((rx & 0xE0) == CMD_WREG) {
        emu->reg_addr = rx & 0x1F;
        if (emu->reg_addr >= ADS1261_EMU_REG_COUNT) {
          emu->reg_addr = ADS1261_EMU_REG_COUNT - 1u;
        }
        spi_put(emu, 0x00);
        emu->state = EMU_STATE_WREG_COUNT;

      } else {
        // NOP ou commande inconnue
        spi_put(emu, 0x00);
      }
      break;

    // ── RREG ──────────────────────────────────────────────────────────────
    case EMU_STATE_RREG_COUNT:
      // rx = nombre de registres - 1
      emu->remaining = rx;
      spi_put(emu, emu->regs[emu->reg_addr]);  // Pré-charger premier registre
      emu->state = EMU_STATE_RREG_DATA;
      break;

    case EMU_STATE_RREG_DATA:
      // regs[reg_addr] vient d'être envoyé; préparer le suivant
      if (emu->remaining > 0u) {
        emu->reg_addr++;
        emu->remaining--;
        uint8_t val = (emu->reg_addr < ADS1261_EMU_REG_COUNT)
                          ? emu->regs[emu->reg_addr]
                          : 0x00u;
        spi_put(emu, val);
      } else {
        spi_put(emu, 0x00);
        emu->state = EMU_STATE_IDLE;
      }
      break;

    // ── WREG ──────────────────────────────────────────────────────────────
    case EMU_STATE_WREG_COUNT:
      emu->remaining = rx;
      spi_put(emu, 0x00);
      emu->state = EMU_STATE_WREG_DATA;
      break;

    case EMU_STATE_WREG_DATA:
      // REG_ID (0x00) est en lecture seule
      if (emu->reg_addr != REG_ID && emu->reg_addr < ADS1261_EMU_REG_COUNT) {
        if (emu->strict_mode && emu->reg_addr == REG_MODE3 &&
            (rx & MODE3_STATUS_CRC_MASK) != 0u) {
          warn_flags |= ADS1261_EMU_TRACE_WARN_UNSUPPORTED_FEATURE;
          aux = rx;
        }
        emu->regs[emu->reg_addr] = rx;
      }
      spi_put(emu, 0x00);
      if (emu->remaining > 0u) {
        emu->reg_addr++;
        emu->remaining--;
      } else {
        emu->state = EMU_STATE_IDLE;
      }
      break;

    // ── RDATA ─────────────────────────────────────────────────────────────
    case EMU_STATE_RDATA_B1:
      spi_put(emu, (uint8_t)((emu->adc_value >> 8) & 0xFF));
      emu->state = EMU_STATE_RDATA_B2;
      break;

    case EMU_STATE_RDATA_B2:
      spi_put(emu, (uint8_t)(emu->adc_value & 0xFF));
      emu->state = EMU_STATE_RDATA_B3;
      break;

    case EMU_STATE_RDATA_B3:
      spi_put(emu, 0x00);
      // Simuler la prochaine conversion prête avec délai optionnel.
      if (emu->conversion_running) {
        if (emu->drdy_delay_ticks == 0u) {
          emu->regs[REG_STATUS] |= STATUS_DRDY;
          emu->drdy_countdown = 0u;
        } else {
          emu->regs[REG_STATUS] &= (uint8_t)~STATUS_DRDY;
          emu->drdy_countdown = emu->drdy_delay_ticks;
        }
      }
      emu->state = EMU_STATE_IDLE;
      break;

    default:
      spi_put(emu, 0x00);
      emu->state = EMU_STATE_IDLE;
      break;
  }

  trace_push(rx, g_last_tx, state_before, (uint8_t)emu->state, warn_flags, aux);
}

bool ADS1261_Emulator_PopTrace(ADS1261_Emulator_t *emu,
                               ADS1261_Emulator_Trace_t *out) {
  bool has_data;
  (void)emu;
  __disable_irq();
  has_data = (g_trace_rd != g_trace_wr);
  if (has_data) {
    *out = g_trace_buf[g_trace_rd];
    g_trace_rd = (uint16_t)((g_trace_rd + 1u) % EMU_TRACE_BUF_SIZE);
  }
  __enable_irq();
  return has_data;
}