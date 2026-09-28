# Hardware notes: 1.54" ESP8684 weather cube

## Chip
- **ESP8684H4** (ESP32-C2) rev v2.0, 4 MB embedded GD flash
- **26 MHz crystal**: firmware must be built with `CONFIG_XTAL_FREQ_26=y`
- ROM boot log appears at **74880 baud** because of the 26 MHz crystal. The app log is at 115200.
- USB-C → **CH340C** USB-serial, with a two-transistor auto-reset circuit, so `esptool` enters download mode by itself
- Stock firmware: ESP-IDF v5.5.4, flash encryption **off**

## Display: ST7789, 240×240, 1.54"
| Signal    | GPIO | Notes |
|-----------|------|-------|
| MOSI/SDA  | 6    | SPI2_HOST |
| SCLK      | 4    | |
| CS        | none | tied low on the board |
| DC        | 5    | |
| RST       | 1    | low 100 ms, high 100 ms |
| Backlight | 18   | LEDC PWM, active high |

- SPI **mode 3**, 40 MHz
- Init: `SLPOUT` → wait 150 ms → `COLMOD 0x55` (RGB565) → `MADCTL 0x00` → `INVON` (the panel needs inversion) → `NORON` → `DISPON`
- No row or column offset

## Inputs
None. The stock firmware only configures DC and RST as GPIOs. Spare GPIOs if you want to add a button: 0, 2, 3, 7, 8 (strapping), 9 (BOOT strapping), 10.
UART0 (GPIO19 RX / GPIO20 TX) goes to the CH340C.

## Manual download mode (if auto-reset ever fails)
Hold **GPIO9 (QFN pin 15)** to GND while plugging in USB. The big pad under the chip is GND.

## How the pinout was found
No schematic was available, so the pins were recovered from a flash dump of the stock firmware:
1. `esptool read-flash` dumped the 4 MB flash. It was plaintext, with the app at 0x10000.
2. The app image was split into its load segments and the code segment disassembled as RISC-V.
3. The stock app references `st7789_init` / `Backlight_pwm_init`. The code near those strings fills `spi_bus_config_t` (MOSI=6, SCLK=4), `spi_device_interface_config_t` (mode 3, 40 MHz, CS=-1), `gpio_config_t` (bit mask 0x22 = GPIO1 + GPIO5) and `ledc_channel_config_t` (GPIO18).
4. The SPI pre-transfer callback drives GPIO5, so that's DC. The reset routine toggles GPIO1, so that's RST.
