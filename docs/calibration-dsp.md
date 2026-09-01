# Calibration DSP and MCP API

This page is the machine-facing contract for the generic I²S calibration path. It is compiled when `CONFIG_AUDIO_CALIBRATION_DSP=y`; the ESP32-S3 defaults enable it for a PCM5102A-style build.

## Signal contract

- Interleaved stereo input and output
- 44.1 or 48 kHz profile sample rate; it must match the firmware output rate
- Ten filters per channel
- Filter types: `PK`, `LS`, `HS`, `HP`/`HPF`, `LP`/`LPF`
- Frequency: 10 Hz to 45% of sample rate
- Gain: -18 to +12 dB per filter
- Q: 0.1 to 20
- Channel gain: -18 to +6 dB
- Channel delay: 0 to 10 ms
- Polarity: `1` or `-1`
- Requested preamp: -30 to +6 dB
- Headroom margin: 0 to 6 dB
- Limiter ceiling: -12 to 0 dBFS
- Limiter look-ahead: 0.5 to 5 ms
- Limiter release: 10 to 1000 ms
- Profile-bound output latency trim: -250,000 to +250,000 µs

Profiles are versioned. Version `3` adds output-latency trim for measured multi-room alignment. Version 2 profiles already stored in NVS are migrated with a zero trim; measurement mode always forces volume-dependent loudness off.

## Endpoints

### `GET /api/dsp/capabilities`

Call this before generating a profile. The response is authoritative for the running firmware and includes the target name, API/profile versions, sample rate, channel count, supported filter types and limits.

### `GET /api/dsp/profile`

Returns the saved profile plus runtime values that can differ from the request:

- `effective_preamp_db`: requested preamp after automatic cascade headroom
- `cascade_peak_db`: highest sampled response across both channel cascades
- `profile_hash`: FNV-1a hash of the accepted profile blob
- `bypassed`: current runtime bypass state
- `output_latency_trim_us`: signed acoustic/output timing correction

### `PUT /api/dsp/profile`

Accepts the shape in [`examples/dsp-profile.json`](../examples/dsp-profile.json). Both `left` and `right` channel objects are required. The operation is transactional from the caller's point of view:

1. copy the current profile to the rollback slot;
2. validate and activate the proposed profile;
3. persist it to NVS;
4. restore the old active profile if persistence fails.

An HTTP `422` means validation or persistence failed. Do not measure until the returned `profile_hash` matches a fresh `GET /api/dsp/profile`.

### `POST /api/dsp/rew`

The request body is plain REW generic filter text. Supported examples:

```text
Preamp: -5.8 dB
Filter 1: ON PK Fc 47.2 Hz Gain -5.3 dB Q 4.1
Filter 2: ON LS Fc 105 Hz Gain 1.5 dB Q 0.7
Filter 3: ON HP Fc 28 Hz Q 0.707
```

Up to ten recognized filters are applied identically to left and right. Existing channel gain, delay, polarity and limiter settings are preserved. Use `PUT /api/dsp/profile` for independent L/R corrections.

### `POST /api/dsp/bypass`

JSON body: `{"bypass":true}` or `{"bypass":false}`. Bypass is runtime-only and leaves the saved profile untouched, which makes it suitable for a quick A/B check.

### `POST /api/dsp/rollback`

Restores the previous NVS profile. Rollback and current profiles swap, so a second rollback can undo the first.

### `GET /api/dsp/metrics`

Returns:

- profile hash and sample rate;
- processed and limited frame counts;
- samples that exceeded full scale before final safety clamping;
- current limiter gain reduction;
- effective preamp and combined cascade peak;
- smoothed and maximum DSP load;
- I²S output underrun count;
- fixed limiter latency reported to AirPlay timing;
- bypass state.
- profile/bypass transition state;
- measurement session, safe test-tone and sync-test state.

### Profiles, measurement and health

- `GET /api/dsp/profiles` lists eight flash-backed slots.
- `POST /api/dsp/profile/save`, `load` and `delete` accept `{"slot":0}` through `{"slot":7}`.
- `POST /api/dsp/measurement` accepts `enabled`, `fixed_volume_db` and `expected_profile_hash`. A hash mismatch returns `409`.
- `POST /api/audio/test-tone` accepts a 20–20,000 Hz tone, -60 to -12 dBFS, channel mask 1/2/3 and at most 30 seconds. It refuses to start while AirPlay is playing.
- `POST /api/audio/sync-test` generates a Hann-windowed 2 kHz marker burst for acoustic correlation. It accepts a 250–5,000 ms interval, 5–50 ms pulse, -60 to -18 dBFS and a maximum duration of 60 seconds.
- `GET /api/audio/health` reports codec, buffering, packet counters, RTP/PTP health, memory, resets and recent faults.
- `GET /api/now-playing` reports playback state, sender, title, artist, album, progress and stream format. `GET /api/now-playing/artwork` serves the PSRAM-only 192 KiB bounded artwork cache with immutable revision URLs.
- `GET /api/security/status` returns only a token hint. `POST /api/security/reveal` returns the token only while the physical BOOT button is held.
- `GET /api/mqtt/status` and protected `PUT /api/mqtt/config` manage trusted-LAN Home Assistant discovery. Discovery includes a real `media_player` with state, volume, mute, metadata, artwork and DACP-backed transport controls where the sender supports them.

## MCP apply/verify rules

An `esp32_airplay` target should use this sequence:

1. read capabilities and reject an incompatible sample rate or filter count;
2. read and retain the current profile;
3. send the proposed profile;
4. read it back and compare the returned hash;
5. confirm `clipped_samples` and `output_underruns` are not increasing abnormally;
6. when multi-room alignment is requested, run the sync marker, measure the relative arrival time, store `output_latency_trim_us`, then verify it with a second capture;
7. run the post-EQ REW measurement;
8. retain the profile only if the measured result passes the MCP's improvement criteria;
9. call rollback when it does not.

Calibration should remain cut-first and should not boost spatial nulls. Firmware validation protects ranges and headroom; it cannot decide whether a correction is acoustically sane.

## Network safety

Every mutating endpoint requires the random device token in `X-AirPlay-Token` or an `Authorization: Bearer` header. The token is generated on first boot, stored in NVS and printed once in the USB log. Read-only telemetry and captive-portal Wi-Fi setup remain open.

If the first log was missed, hold the board's **BOOT** button and select **Hold BOOT + Reveal** in either web page. The response is marked `no-store`; physical access is the recovery credential.

Transport is plain HTTP and MQTT is intentionally limited to `mqtt://`, so this remains a trusted-LAN appliance. Do not port-forward it or place it on an untrusted network.
