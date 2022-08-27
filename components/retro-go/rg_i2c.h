#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum
{
    RG_I2C_NONE = 0,
    RG_I2C_AW9523,
    RG_I2C_MCP23008,
    RG_I2C_MCP23017,
} rg_i2c_gpio_t;

bool rg_i2c_init(void);
bool rg_i2c_deinit(void);
bool rg_i2c_read(uint8_t addr, int reg, void *read_data, size_t read_len);
bool rg_i2c_write(uint8_t addr, int reg, const void *write_data, size_t write_len);
uint8_t rg_i2c_read_byte(uint8_t addr, uint8_t reg);
bool rg_i2c_write_byte(uint8_t addr, uint8_t reg, uint8_t value);

// GPIO extender
bool rg_i2c_gpio_init(rg_i2c_gpio_t device_type, uint8_t addr);
bool rg_i2c_gpio_deinit(void);
bool rg_i2c_gpio_setup_port(int port, uint16_t config);
bool rg_i2c_gpio_set_level(int pin, int level);
int  rg_i2c_gpio_get_level(int pin);
uint16_t rg_i2c_gpio_read_port(int port);
bool rg_i2c_gpio_write_port(int port, uint16_t value);


#define AW9523_DEFAULT_ADDR 0x58
#define AW9523_REG_CHIPID 0x10     ///< Register for hardcode chip ID
#define AW9523_REG_SOFTRESET 0x7F  ///< Register for soft resetting
#define AW9523_REG_INPUT0 0x00     ///< Register for reading input values
#define AW9523_REG_OUTPUT0 0x02    ///< Register for writing output values
#define AW9523_REG_CONFIG0 0x04    ///< Register for configuring direction
#define AW9523_REG_INTENABLE0 0x06 ///< Register for enabling interrupt
#define AW9523_REG_GCR 0x11        ///< Register for general configuration
#define AW9523_REG_LEDMODE 0x12    ///< Register for configuring const current
