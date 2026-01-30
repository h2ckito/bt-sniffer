#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_gpio.h>
#include <furi_hal_spi.h>
#include "nrf24_driver.h"

// Pines - AJUSTAR SEGÚN TU CONEXIÓN
#define CE_PIN   &gpio_ext_pa7
#define CSN_PIN  &gpio_ext_pa4

// Registros NRF24
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D

#define CMD_R_REG       0x00
#define CMD_W_REG       0x20
#define CMD_R_RX_PL     0x61
#define CMD_FLUSH_RX    0xE2
#define CMD_R_RX_PL_WID 0x60
#define CMD_NOP         0xFF

static bool initialized = false;
static FuriHalSpiBusHandle* spi = NULL;

static uint8_t spi_xfer(uint8_t data) {
    uint8_t rx = 0;
    furi_hal_spi_bus_trx(spi, &data, &rx, 1, 100);
    return rx;
}

static uint8_t read_reg(uint8_t reg) {
    furi_hal_gpio_write(CSN_PIN, false);
    spi_xfer(CMD_R_REG | reg);
    uint8_t val = spi_xfer(CMD_NOP);
    furi_hal_gpio_write(CSN_PIN, true);
    return val;
}

static void write_reg(uint8_t reg, uint8_t val) {
    furi_hal_gpio_write(CSN_PIN, false);
    spi_xfer(CMD_W_REG | reg);
    spi_xfer(val);
    furi_hal_gpio_write(CSN_PIN, true);
}

bool nrf24_init(void) {
    if(initialized) return true;
    
    furi_hal_gpio_init_simple(CE_PIN, GpioModeOutputPushPull);
    furi_hal_gpio_init_simple(CSN_PIN, GpioModeOutputPushPull);
    furi_hal_gpio_write(CE_PIN, false);
    furi_hal_gpio_write(CSN_PIN, true);
    
    spi = &furi_hal_spi_bus_handle_external;
    furi_hal_spi_acquire(spi);
    
    furi_delay_ms(10);
    
    write_reg(REG_EN_AA, 0x00);
    write_reg(REG_EN_RXADDR, 0x3F);
    write_reg(REG_SETUP_AW, 0x03);
    write_reg(REG_RF_CH, 76);
    write_reg(REG_RF_SETUP, 0x0F);
    write_reg(REG_DYNPD, 0x3F);
    write_reg(REG_FEATURE, 0x07);
    
    furi_hal_gpio_write(CSN_PIN, false);
    spi_xfer(CMD_FLUSH_RX);
    furi_hal_gpio_write(CSN_PIN, true);
    write_reg(REG_STATUS, 0x70);
    
    write_reg(REG_CONFIG, 0x0F);
    furi_hal_gpio_write(CE_PIN, true);
    
    furi_delay_ms(5);
    initialized = true;
    return true;
}

void nrf24_deinit(void) {
    if(!initialized) return;
    furi_hal_gpio_write(CE_PIN, false);
    write_reg(REG_CONFIG, 0x00);
    furi_hal_spi_release(spi);
    initialized = false;
}

bool nrf24_available(void) {
    if(!initialized) return false;
    return (read_reg(REG_STATUS) & 0x40) != 0;
}

bool nrf24_read(uint8_t* buf, uint8_t* len, uint8_t* pipe) {
    if(!initialized) return false;
    
    uint8_t status = read_reg(REG_STATUS);
    if(!(status & 0x40)) return false;
    
    *pipe = (status >> 1) & 0x07;
    
    furi_hal_gpio_write(CSN_PIN, false);
    spi_xfer(CMD_R_RX_PL_WID);
    *len = spi_xfer(CMD_NOP);
    furi_hal_gpio_write(CSN_PIN, true);
    
    if(*len > 32) *len = 32;
    
    furi_hal_gpio_write(CSN_PIN, false);
    spi_xfer(CMD_R_RX_PL);
    for(uint8_t i = 0; i < *len; i++) {
        buf[i] = spi_xfer(CMD_NOP);
    }
    furi_hal_gpio_write(CSN_PIN, true);
    
    write_reg(REG_STATUS, 0x40);
    return true;
}

void nrf24_set_channel(uint8_t ch) {
    if(ch > 125) ch = 125;
    write_reg(REG_RF_CH, ch);
}
