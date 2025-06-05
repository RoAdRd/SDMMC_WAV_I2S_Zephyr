# WAV Player Application

This is a WAV audio player application built for Zephyr RTOS. The application reads WAV files from an SD card and plays them through an I2S interface.

## Features

- SD card support via SPI interface
- WAV file playback through I2S
- Supports 8-bit and 16-bit PCM WAV files
- Supports mono and stereo audio (up to 2 channels)
- FAT filesystem support for SD card

## Hardware Requirements

- Nordic Semiconductor nRF device with I2S support
- SD card module connected via SPI
- Audio DAC connected to I2S interface

## Pin Configuration

The application uses the following pin configuration:

- SPI4 for SD card:
  - SCK: P0.24
  - MISO: P0.23
  - MOSI: P0.22
  - CS: P0.25

- I2S0 for audio:
  - SCK: P1.15
  - LRCK: P1.12
  - SDOUT: P1.13

## Building and Running

1. Configure your project with the required Kconfig options:
   - Enable `CONFIG_FAT_FILESYSTEM_ELM` for SD card support
   - Configure I2S and SPI interfaces

2. Place your WAV file on the SD card as `test.wav`

3. Build and flash the application:
   ```bash
   west build
   west flash
   ```

## WAV File Requirements

- Format: PCM
- Sample rate: Any (configured in WAV header)
- Bit depth: 8 or 16 bits
- Channels: 1 (mono) or 2 (stereo)

## Limitations

- Maximum 2 audio channels
- Only supports PCM WAV format
- Fixed buffer size for I2S transmission
- SD card must be formatted with FAT filesystem

## Dependencies

- Zephyr RTOS
- FAT filesystem support
- I2S driver
- SPI driver
- SD card driver

## License

[Add your license information here]
