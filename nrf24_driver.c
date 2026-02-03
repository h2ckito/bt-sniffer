// NRF24 Driver - Compatible con firmware API 38
// Basado en la implementación de NRF24 Scanner/Sniffer
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_gpio.h>
#include <furi_hal_spi.h>
#include <furi_hal_resources.h>
#include "nrf24_driver.h"

// Pines estándar NRF24 para Flipper Zero
#define NRF24_CE_PIN    &gpio_ext_pb2
#define NRF24_CSN_PIN   &gpio_ext_pa4

// Registros NRF24
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RX_ADDR_P0  0x0A
#define REG_RX_PW_P0    0x11
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D

#define CMD_R_REGISTER    0x00
#define CMD_W_REGISTER    0x20
#define CMD_R_RX_PAYLOAD  0x61
#define CMD_FLUSH_TX      0xE1
#define CMD_FLUSH_RX      0xE2
#define CMD_NOP           0xFF

#define FLAG_RX_DR   0x40
#define FLAG_TX_DS   0x20
#define FLAG_MAX_RT  0x10

static bool nrf24_initialized = false;
static FuriHalSpiBusHandle* spi_handle = NULL;

// Función SPI de bajo nivel
static uint8_t nrf24_spi_trx(uint8_t* tx, uint8_t* rx, uint8_t len) {
    furi_hal_gpio_write(NRF24_CSN_PIN, false);
    furi_delay_us(1);
    
    if(tx != NULL && rx != NULL) {
        furi_hal_spi_bus_trx(spi_handle, tx, rx, len, 1000);
    } else if(tx != NULL) {
        furi_hal_spi_bus_tx(spi_handle, tx, len, 1000);
    } else if(rx != NULL) {
        furi_hal_spi_bus_rx(spi_handle, rx, len, 1000);
    }
    
    furi_delay_us(1);
    furi_hal_gpio_write(NRF24_CSN_PIN, true);
    
    return (rx != NULL) ? rx[0] : 0;
}

static uint8_t nrf24_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = {CMD_W_REGISTER | reg, value};
    uint8_t rx[2] = {0, 0};
    nrf24_spi_trx(tx, rx, 2);
    return rx[0];
}

static uint8_t nrf24_read_reg(uint8_t reg) {
    uint8_t tx[2] = {CMD_R_REGISTER | reg, 0xFF};
    uint8_t rx[2] = {0, 0};
    nrf24_spi_trx(tx, rx, 2);
    return rx[1];
}

static uint8_t nrf24_get_status(void) {
    uint8_t tx = CMD_NOP;
    uint8_t rx = 0;
    furi_hal_gpio_write(NRF24_CSN_PIN, false);
    furi_delay_us(1);
    furi_hal_spi_bus_trx(spi_handle, &tx, &rx, 1, 1000);
    furi_hal_gpio_write(NRF24_CSN_PIN, true);
    return rx;
}

static void nrf24_write_addr(uint8_t reg, uint8_t* addr, uint8_t len) {
    uint8_t tx[6];
    uint8_t rx[6];
    tx[0] = CMD_W_REGISTER | reg;
    for(uint8_t i = 0; i < len && i < 5; i++) {
        tx[i + 1] = addr[i];
    }
    nrf24_spi_trx(tx, rx, len + 1);
}

static void nrf24_flush(void) {
    uint8_t cmd;
    
    cmd = CMD_FLUSH_TX;
    furi_hal_gpio_write(NRF24_CSN_PIN, false);
    furi_hal_spi_bus_tx(spi_handle, &cmd, 1, 1000);
    furi_hal_gpio_write(NRF24_CSN_PIN, true);
    
    furi_delay_us(10);
    
    cmd = CMD_FLUSH_RX;
    furi_hal_gpio_write(NRF24_CSN_PIN, false);
    furi_hal_spi_bus_tx(spi_handle, &cmd, 1, 1000);
    furi_hal_gpio_write(NRF24_CSN_PIN, true);
}

bool nrf24_init(void) {
    if(nrf24_initialized) return true;
    
    FURI_LOG_I("NRF24", "Initializing...");
    
    // Configurar pines GPIO
    furi_hal_gpio_init(NRF24_CE_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_init(NRF24_CSN_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    
    // Estado inicial de pines
    furi_hal_gpio_write(NRF24_CE_PIN, false);
    furi_hal_gpio_write(NRF24_CSN_PIN, true);
    
    // Obtener handle SPI
    spi_handle = &furi_hal_spi_bus_handle_external;
    furi_hal_spi_acquire(spi_handle);
    
    furi_delay_ms(5);
    
    // Power down y reset
    nrf24_write_reg(REG_CONFIG, 0x00);
    furi_delay_ms(2);
    
    // Configuración para sniffing (igual que NRF24 Scanner)
    nrf24_write_reg(REG_EN_AA, 0x00);       // Deshabilitar Auto-ACK en todos los pipes
    nrf24_write_reg(REG_EN_RXADDR, 0x03);   // Habilitar pipe 0 y 1
    nrf24_write_reg(REG_SETUP_AW, 0x03);    // 5 bytes de dirección
    nrf24_write_reg(REG_SETUP_RETR, 0x00);  // Sin retransmisiones
    nrf24_write_reg(REG_RF_CH, 2);          // Canal 2 inicial
    nrf24_write_reg(REG_RF_SETUP, 0x26);    // 250kbps, 0dBm (Logitech usa 250kbps)
    nrf24_write_reg(REG_RX_PW_P0, 32);      // Payload 32 bytes pipe 0
    nrf24_write_reg(REG_RX_PW_P0 + 1, 32);  // Payload 32 bytes pipe 1
    nrf24_write_reg(REG_DYNPD, 0x00);       // Sin dynamic payload
    nrf24_write_reg(REG_FEATURE, 0x00);     // Sin features
    
    // Dirección para escaneo promiscuo
    uint8_t addr[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    nrf24_write_addr(REG_RX_ADDR_P0, addr, 5);
    
    // Limpiar FIFOs
    nrf24_flush();
    
    // Limpiar flags de status
    nrf24_write_reg(REG_STATUS, FLAG_RX_DR | FLAG_TX_DS | FLAG_MAX_RT);
    
    // Power up en modo RX
    nrf24_write_reg(REG_CONFIG, 0x0F); // PWR_UP + PRIM_RX + CRC enabled
    furi_delay_ms(2);
    
    // Activar recepción
    furi_hal_gpio_write(NRF24_CE_PIN, true);
    furi_delay_us(150);
    
    // Verificar que el chip responde
    uint8_t config = nrf24_read_reg(REG_CONFIG);
    if(config == 0x00 || config == 0xFF) {
        FURI_LOG_E("NRF24", "Chip not responding! Config=0x%02X", config);
        furi_hal_spi_release(spi_handle);
        return false;
    }
    
    FURI_LOG_I("NRF24", "Init OK, Config=0x%02X", config);
    nrf24_initialized = true;
    return true;
}

void nrf24_deinit(void) {
    if(!nrf24_initialized) return;
    
    FURI_LOG_I("NRF24", "Deinitializing...");
    
    furi_hal_gpio_write(NRF24_CE_PIN, false);
    nrf24_write_reg(REG_CONFIG, 0x00);
    furi_hal_spi_release(spi_handle);
    
    nrf24_initialized = false;
}

bool nrf24_available(void) {
    if(!nrf24_initialized) return false;
    
    uint8_t status = nrf24_get_status();
    return (status & FLAG_RX_DR) != 0;
}

bool nrf24_read(uint8_t* buf, uint8_t* len, uint8_t* pipe) {
    if(!nrf24_initialized || buf == NULL || len == NULL) return false;
    
    uint8_t status = nrf24_get_status();
    
    // Verificar si hay datos disponibles
    if(!(status & FLAG_RX_DR)) {
        return false;
    }
    
    // Obtener pipe
    if(pipe != NULL) {
        *pipe = (status >> 1) & 0x07;
    }
    
    // Leer payload (32 bytes fijos)
    *len = 32;
    
    uint8_t tx[33];
    uint8_t rx[33];
    tx[0] = CMD_R_RX_PAYLOAD;
    for(uint8_t i = 1; i < 33; i++) tx[i] = 0xFF;
    
    nrf24_spi_trx(tx, rx, 33);
    
    for(uint8_t i = 0; i < 32; i++) {
        buf[i] = rx[i + 1];
    }
    
    // Limpiar flag RX_DR
    nrf24_write_reg(REG_STATUS, FLAG_RX_DR);
    
    return true;
}

void nrf24_set_channel(uint8_t ch) {
    if(!nrf24_initialized) return;
    if(ch > 125) ch = 125;
    
    // Desactivar CE para cambiar canal
    furi_hal_gpio_write(NRF24_CE_PIN, false);
    furi_delay_us(10);
    
    nrf24_write_reg(REG_RF_CH, ch);
    
    // Reactivar CE
    furi_hal_gpio_write(NRF24_CE_PIN, true);
    furi_delay_us(150);
}
