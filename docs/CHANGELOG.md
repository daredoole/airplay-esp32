# Changelog

## Unreleased

- Add the exact ESP32-S3 N16R8 target, octal PSRAM configuration, crash partition and OTA rollback health gate.
- Sign PlatformIO's final application binary in the secure-OTA environment.
- Add authenticated mutations with a per-device token shown once over USB.
- Add physical BOOT-button token recovery without weakening LAN authentication.
- Add eight DSP profile slots, click-free switching, measurement mode, safe channel test tone and optional volume-dependent loudness.
- Keep DSP in float through a stereo look-ahead limiter, then TPDF-dither to 24-bit audio in 32-bit I²S slots.
- Add audio/PTP/RTP/crash diagnostics and Home Assistant MQTT discovery.
- Replace the calibration page with a responsive control-and-verification console.
