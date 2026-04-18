#pragma once

#include "dac.h"

/**
 * TI TLV320AIC3204 DAC driver ops — register with dac_register() before
 * calling dac_init(). See components/dac/include/dac.h for the op contract.
 *
 * Wiring on HA Voice PE (reference: docs/home-assistant-voice-pe-dev/…yaml):
 *   - I2C address 0x18 (via board I2C master on GPIO 5 SDA / 6 SCL)
 *   - I2S master-out from ESP32-S3 on BCK=8 LRCLK=7 DOUT=10
 *   - Line-out routes to the 3.5 mm jack (→ Denon AUX)
 *   - HP/SPK route feeds the TPA6211A internal amp (enabled via GPIO47
 *     handled in the board layer; this driver does not touch GPIO47).
 */
extern const dac_ops_t dac_tlv320aic3204_ops;
