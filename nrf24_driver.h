// NRF24 Driver Header - Usa la librería del firmware
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Funciones públicas
bool nrf24_init(void);
void nrf24_deinit(void);
bool nrf24_available(void);
bool nrf24_read(uint8_t* buf, uint8_t* len, uint8_t* pipe);
void nrf24_set_channel(uint8_t ch);
