# ESP32-S3 + PCM5102A

The cheapest and most common build: a generic ESP32-S3 dev board with an external
PCM5102A I2S DAC plugged straight onto its pins. This fork's default build environment
(`esp32s3`) targets the connected N16R8 module: 16 MB flash and 8 MB octal PSRAM.

For the parts list and step-by-step assembly, see
[shopping list](../getting-started/shopping-list.md) and
[assembly](../getting-started/assembly.md).

## Default I2S pins

| Function | ESP32-S3 pin | PCM5102A pin |
| --- | --- | --- |
| Bit clock | 11 | BCK |
| Audio data | 12 | DIN |
| Word select (LRCLK) | 13 | LCK |
| Master clock reference | GND | SCK |
| Ground | Any pin marked GND | GND |
| Power | 3V3 | VIN |

MCLK is not needed by the PCM5102A because it generates the clock internally;
ground `SCK` for this tested build. GPIO14 is not ground. Use a real ESP32 pin
marked `GND`—all such ground pins are electrically common.

Pins can be changed under **Board Configuration → Pin Configuration** in `menuconfig`.

!!! warning "Use 3V3 on this exact board"

    The tested YD-ESP32-S3 N16R8 board labels its USB supply pin `5VIN`; that pin
    is an input, not a convenient 5 V output. Connect PCM5102A `VIN` to `3V3`.
    This is the wiring that was verified working on the physical receiver.

## PCM5102A mode jumpers

Set the four three-pad jumpers to **H1L, H2L, H3H, H4L**. Join the center pad
to only the named side. Never cover all three pads with solder.

![PCM5102A solder jumper map](../assets/pcm5102a-jumper-map.svg)

H3 is `XSMT`: H3H holds the DAC out of hardware mute. The firmware therefore
uses its software transition fade unless you separately wire `XSMT` to a GPIO.

## Flashing

=== "Browser"

    Use the installer on the [flashing page](../getting-started/flashing.md).

=== "PlatformIO"

    ```bash
    pio run -e esp32s3 -t upload
    pio run -e esp32s3 -t uploadfs
    ```

    The second command is required — it writes the web UI to SPIFFS.

    Use `esp32s3-secure-ota` after generating a private signing key when you
    want signed web updates and automatic boot rollback.

## Calibrated output path

The S3 build keeps the DSP path in float through the limiter, applies TPDF
dither, and sends effective 24-bit samples in 32-bit I2S slots. Open `/dsp`
for profiles, REW import, measurement mode, diagnostics and MQTT discovery.
Profile changes fade over 50 ms to avoid clicks.

## Pop-free XSMT / amplifier mute

The output now boots muted, keeps mute asserted while I²S is stopped or
reconfigured, queues a faded-in buffer after clocks stabilize, and performs the
reverse sequence when playback ends. To use it, connect the PCM5102A `XSMT`
pin—or an amplifier mute/enable input—to an otherwise unused GPIO, then set:

```ini
CONFIG_MUTE_GPIO=YOUR_GPIO
CONFIG_MUTE_GPIO_LEVEL=0
```

`0` is correct for PCM5102A XSMT because low means mute. Leave the GPIO at `-1`
until that wire exists; the sequence remains active in software without driving
an unknown pin.

The S3 build also accepts bounded artwork by default. Images larger than 192 KiB
are rejected and accepted images live only in PSRAM, away from the audio/DMA
heap. `/dsp` shows title, artist, album, sender, format, progress and artwork.

=== "ESP-IDF"

    ```bash
    idf.py set-target esp32s3
    idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s3" build
    idf.py -p /dev/ttyUSB0 flash
    ```

## No Bluetooth on the S3

The ESP32-S3 has Bluetooth LE only, not Bluetooth Classic, so A2DP audio is not available.
The `esp32s3` build contains no Bluetooth support at all. Use an ESP32-based board such as
the [SqueezeAMP](squeezeamp.md) or [Esparagus Audio Brick](esparagus-audio-brick.md) if you
want to stream over Bluetooth.

## Variants

### Waveshare ESP32-S3

Waveshare's ESP32-S3 boards use a different pin arrangement and have their own environment
and prebuilt binary:

```bash
pio run -e waveshare-esp32s3 -t upload
pio run -e waveshare-esp32s3 -t uploadfs
```

### JTAG debugging

`esp32s3-jtag` extends the `esp32s3` environment and uploads over the built-in USB JTAG
bridge instead of the serial bootloader:

```bash
pio run -e esp32s3-jtag -t upload
```

### ESP32-S2

An ESP32-S2 build is produced in CI and published as
`airplay2-receiver-esp32s2.bin`, flashable from the
[browser installer](../getting-started/flashing.md). There is no PlatformIO environment for
it — build it through ESP-IDF:

```bash
idf.py set-target esp32s2
idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esp32s2" build
```

### ESP32-WROVER

For older WROVER modules with 4 MB flash, `esp32wrover-dev` targets the Freenove WROVER
board and includes Bluetooth:

```bash
pio run -e esp32wrover-dev -t upload
```
