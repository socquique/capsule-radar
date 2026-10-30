// ST77916 bring-up for the Waveshare ESP32-S3-Touch-LCD-1.85C V2. See panel_st77916.h.
// The reset, the ID read and the ID check are adapted from Hermes Gadget SDK (MIT,
// https://github.com/Adolanium/hermes-gadget-sdk, firmware/esp32/main/port_display.cpp),
// which follows Waveshare's factory demo. The init tables are in st77916_ws185_init.h.
#include "config.h"
#if defined(BOARD_LCD185C_V2)
#include "panel_st77916.h"
#include "st77916_ws185_init.h"
#include "tca9554.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>

// Command 0x04 sent through the QSPI read opcode 0x0B at 3 MHz, as the factory demo
// does, on a bus that Arduino_ESP32QSPI then sets up again for drawing.
static bool read_panel_id(uint8_t id[4]) {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_SCLK;
    bus.data0_io_num = PIN_LCD_D0;
    bus.data1_io_num = PIN_LCD_D1;
    bus.data2_io_num = PIN_LCD_D2;
    bus.data3_io_num = PIN_LCD_D3;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = 64;
    bus.flags = SPICOMMON_BUSFLAG_QUAD;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = PIN_LCD_CS;
    io_cfg.dc_gpio_num = -1;
    io_cfg.pclk_hz = 3000000;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.trans_queue_depth = 1;
    io_cfg.flags.quad_mode = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    bool ok = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io) == ESP_OK;
    if (ok) {
        ok = esp_lcd_panel_io_rx_param(io, (0x0Bu << 24) | (0x04u << 8), id, 4) == ESP_OK;
        esp_lcd_panel_io_del(io);
    }
    spi_bus_free(SPI2_HOST);
    return ok;
}

Arduino_GFX *st77916_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    tca9554_init();
    tca9554_set(EXIO_LCD_RST, 0);
    delay(10);
    tca9554_set(EXIO_LCD_RST, 1);
    delay(120);

    uint8_t id[4] = {};
    const bool read = read_panel_id(id);
    Serial.printf("[display] ST77916 panel ID %02X %02X %02X %02X\n", id[0], id[1], id[2], id[3]);
    // Waveshare ships two glass revisions with different tables: 00 7F 7F 7F and 00 02 7F 7F.
    if (!read || id[0] != 0 || id[2] != 0x7F || id[3] != 0x7F || (id[1] != 0x7F && id[1] != 0x02)) {
        Serial.println("[display] unknown ST77916 panel revision; not guessing an init table");
        return nullptr;
    }
    const bool newer = id[1] == 0x02;

    Arduino_DataBus *bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK,
                                                 PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    // ips = true: the tables end with INVON, and Arduino_TFT re-sends inversion after them.
    Arduino_GFX *gfx = new Arduino_ST77916(bus, GFX_NOT_DEFINED, 0, true, PANEL_W, PANEL_H, 0, 0, 0, 0,
        newer ? ST77916_WS185_INIT_NEW : ST77916_WS185_INIT_DEFAULT,
        newer ? sizeof(ST77916_WS185_INIT_NEW) : sizeof(ST77916_WS185_INIT_DEFAULT));
    if (!gfx->begin(LCD_QSPI_HZ)) return nullptr;
    ledcAttach(PIN_LCD_BL, LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
    Serial.printf("[display] st77916 %dx%d ready (%s table)\n", PANEL_W, PANEL_H, newer ? "new" : "default");
    return gfx;
}

void st77916_set_brightness(uint8_t v) {
    ledcWrite(PIN_LCD_BL, (uint32_t)v * ((1u << LCD_BL_PWM_BITS) - 1) / 255u);
}

#endif  // BOARD_LCD185C_V2
