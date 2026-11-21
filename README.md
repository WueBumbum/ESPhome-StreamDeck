# ESPHome Stream Deck Module 6-Key – experimental proof of concept

**Status: source prototype; not yet compiled against a pinned ESPHome version and not hardware-tested.**

Target: YD-ESP32-S3 2002-V1.3 / ESP32-S3-WROOM-1-N16R8 and Elgato Stream Deck 6-Key Module (VID `0x0FD9`, PID `0x00B8`).

Expected behavior: on USB connection the first key displays a gray power symbol. Each press of the first key switches between gray (OFF) and green (ON). All other keys are ignored. Debug messages appear in ESPHome logs.

## Repository and installation

1. Create a **public** GitHub repository named `esphome-streamdeck-poc` (without adding a README during creation).
2. Upload **the contents of this folder** into the repository root, preserving `components/streamdeck_poc/`.
3. In `streamdeck.yaml`, replace `OWNER` with your GitHub account name. For initial experiments, `ref: main` and `refresh: 0s` are intentional.
4. Copy `secrets.example.yaml` to `secrets.yaml` on the machine where ESPHome builds the firmware, fill in the Wi-Fi credentials, generate a 32-byte base64 encryption key, and choose OTA and fallback passwords. **Do not commit `secrets.yaml` to GitHub.**
5. Copy `streamdeck.yaml` and `secrets.yaml` to the ESPHome Device Builder configuration folder, or build locally with ESPHome CLI. Use the UART USB-C port for flashing and serial logging.
6. Compile, flash and inspect logs **before connecting the Stream Deck**. Connect the Stream Deck to the native USB-OTG port only after confirming 5 V VBUS is available. Many ESP32-S3 boards do not supply VBUS in host mode by themselves.
7. The PoC uses the ESP-IDF USB host stack directly, not ESPHome's `usb_host:` component. Do not enable a second USB host manager concurrently.

## Source layout

- `streamdeck.yaml`: example configuration that fetches this repository as an external component
- `components/streamdeck_poc/__init__.py`: ESPHome integration and component registration
- `components/streamdeck_poc/streamdeck_poc.h/.cpp`: experimental USB host, HID input and BMP output
- `secrets.example.yaml`: local credentials template

## Protocol

Official Elgato documentation: https://docs.elgato.com/streamdeck/hid/mini/

The PoC builds 80x80 24-bit BMP bytes in RAM (including a 54-byte header), rotates the image in software, and sends 1024-byte HID output reports with a 16-byte report header. Input report ID `0x01` reports six button states.

## Important limitations

- **Not verified as compiling or working on the target hardware.** ESP-IDF and ESPHome API differences may require adjustments.
- The USB host class output-report transport uses HID `SET_REPORT` control transfers as an initial implementation. Some firmware revisions may require interrupt OUT instead; check the HID descriptor and logs.
- Disconnect/reconnect cleanup is intentionally incomplete; for the first test, power-cycle the ESP32 after disconnecting the Stream Deck.
- The image toggle currently uses only button 1; this is deliberately not yet integrated into Home Assistant events.
- No web UI, image uploads, filesystem, or OTA release workflow yet.
- The ESP32-S3 USB peripheral cannot operate as USB host unless VBUS power is correctly supplied. Never back-feed two 5 V sources into each other.

## Suggested test report

Send the full ESPHome build error if compilation fails. If compilation succeeds, send boot logs including VID/PID, HID endpoint information, first image transfer result and key presses. This is an iterative hardware PoC, not a guaranteed ready-to-flash binary.
