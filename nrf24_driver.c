#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_gpio.h>
#include <furi_hal_spi.h>
#include "nrf24_driver.h"

// Pines GPIO
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
static FuriHalSpiBusHandle* spi_handle = &furi_hal_spi_bus_handle_external;

static uint8_t nrf24_read_reg(uint8_t reg) {
    uint8_t tx_data[2] = {CMD_R_REG | reg, 0xFF};
    uint8_t rx_data[2] = {0};
    
    furi_hal_gpio_write(CSN_PIN, false);
    furi_hal_spi_bus_trx(spi_handle, tx_data, rx_data, 2, 1000);
    furi_hal_gpio_write(CSN_PIN, true);
    
    return rx_data[1];
}

static void nrf24_write_reg(uint8_t reg, uint8_t data) {
    uint8_t tx_data[2] = {CMD_W_REG | reg, data};
    uint8_t rx_data[2] = {0};
    
    furi_hal_gpio_write(CSN_PIN, false);
    furi_hal_spi_bus_trx(spi_handle, tx_data, rx_data, 2, 1000);
    furi_hal_gpio_write(CSN_PIN, true);
}

static void nrf24_cmd(uint8_t cmd) {
    uint8_t rx;
    furi_hal_gpio_write(CSN_PIN, false);
    furi_hal_spi_bus_trx(spi_handle, &cmd, &rx, 1, 1000);
    furi_hal_gpio_write(CSN_PIN, true);
}

bool nrf24_init(void) {
    if(initialized) return true;
    
    furi_hal_gpio_init(CE_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(CSN_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    
    furi_hal_gpio_write(CE_PIN, false);
    furi_hal_gpio_write(CSN_PIN, true);
    
    furi_hal_spi_acquire(spi_handle);
    
    furi_delay_ms(100);
    
    nrf24_write_reg(REG_CONFIG, 0x00);
    furi_delay_ms(10);
    
    nrf24_write_reg(REG_EN_AA, 0x00);
    nrf24_write_reg(REG_EN_RXADDR, 0x3F);
    nrf24_write_reg(REG_SETUP_AW, 0x03);
    nrf24_write_reg(REG_RF_CH, 25);
    nrf24_write_reg(REG_RF_SETUP, 0x26);
    nrf24_write_reg(REG_DYNPD, 0x3F);
    nrf24_write_reg(REG_FEATURE, 0x04);
    
    nrf24_cmd(CMD_FLUSH_RX);
    nrf24_write_reg(REG_STATUS, 0x70);
    
    nrf24_write_reg(REG_CONFIG, 0x0F);
    furi_delay_ms(5);
    
    furi_hal_gpio_write(CE_PIN, true);
    furi_delay_ms(1);
    
    initialized = true;
    FURI_LOG_I("NRF24", "Initialized");
    return true;
}

void nrf24_deinit(void) {
    if(!initialized) return;
    
    furi_hal_gpio_write(CE_PIN, false);
    nrf24_write_reg(REG_CONFIG, 0x00);
    furi_hal_spi_release(spi_handle);
    
    initialized = false;
}

bool nrf24_available(void) {
    if(!initialized) return false;
    uint8_t status = nrf24_read_reg(REG_STATUS);
    return (status & 0x40) != 0;
}

bool nrf24_read(uint8_t* buf, uint8_t* len, uint8_t* pipe) {
    if(!initialized) return false;
    
    uint8_t status = nrf24_read_reg(REG_STATUS);
    if(!(status & 0x40)) return false;
    
    *pipe = (status >> 1) & 0x07;
    
    uint8_t tx[2] = {CMD_R_RX_PL_WID, 0xFF};
    uint8_t rx[2] = {0};
    furi_hal_gpio_write(CSN_PIN, false);
    furi_hal_spi_bus_trx(spi_handle, tx, rx, 2, 1000);
    furi_hal_gpio_write(CSN_PIN, true);
    
    *len = rx[1];
    if(*len > 32) {
        nrf24_cmd(CMD_FLUSH_RX);
        *len = 0;
        return false;
    }
    
    uint8_t tx_buf[33] = {0};
    uint8_t rx_buf[33] = {0};
    tx_buf[0] = CMD_R_RX_PL;
    
    furi_hal_gpio_write(CSN_PIN, false);
    furi_hal_spi_bus_trx(spi_handle, tx_buf, rx_buf, *len + 1, 1000);
    furi_hal_gpio_write(CSN_PIN, true);
    
    memcpy(buf, &rx_buf[1], *len);
    
    nrf24_write_reg(REG_STATUS, 0x40);
    
    return true;
}

void nrf24_set_channel(uint8_t ch) {
    if(ch > 125) ch = 125;
    furi_hal_gpio_write(CE_PIN, false);
    nrf24_write_reg(REG_RF_CH, ch);
    furi_hal_gpio_write(CE_PIN, true);
}
