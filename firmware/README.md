# PillMate voice reminder firmware

## What is included

- `PillMate_Box_Controller_voice_transfer.ino` retains the existing ESP32-S3 medication schedule, drawer servo, PIR, RTC and PillStrap X ESP-NOW logic.
- Drawer servo pins: Drawer 1 = GPIO17, Drawer 2 = GPIO21, Drawer 3 = GPIO47, Drawer 4 = GPIO48.
- GPIO4 is the voice audio output. The firmware uses LEDC PWM audio output on GPIO4.
- The local setup page accepts one WAV reminder per drawer.
- Uploads are checked for PCM, mono, 16 kHz, 16-bit WAV and stored as `/voice1.wav` through `/voice4.wav` in LittleFS.
- When a scheduled dose is triggered, the matching drawer's saved WAV reminder is played automatically. Playback runs in a separate FreeRTOS task so the controller can continue servicing the web server and ESP-NOW.
- The dashboard converts browser recordings to that WAV format and saves them in Supabase. The saved recording list includes a download link for transfer to the device.

## Flash storage partition

The ESP32-S3-N16R8 uses 16 MB flash. In Arduino IDE, install/select a custom partition scheme based on `partitions_pillmate_16mb.csv` (6 MB application, remaining space for LittleFS). The filesystem partition label must be `spiffs`, because Arduino-ESP32's LittleFS mount uses that label by default.

The firmware intentionally mounts LittleFS with formatting disabled. It will not automatically format or erase existing device storage.

## Transfer workflow

1. Record and save a voice reminder on the dashboard.
2. Download its WAV file.
3. Connect the phone or laptop to `PillMate-S3`.
4. Open `http://192.168.4.1`.
5. Choose the drawer and WAV file in the Voice Reminder Audio section, then upload.

## Playback behavior

- Audio format: PCM, mono, 16 kHz, 16-bit WAV.
- The WAV header is skipped and samples are converted to 8-bit PWM duty values.
- GPIO4 outputs a 62.5 kHz PWM carrier; the saved voice sample is played at 16 kHz.
- A missing recording does not stop dose dispensing, the buzzer, or wristband alert.
- The controller still uses the existing buzzer and wristband repeat alert behavior. The voice file is played when the dose event starts.

## Flashing

Select the ESP32-S3 N16R8 board and the custom 16 MB partition scheme described above, then compile and upload this sketch. The GPIO4 output implementation uses the Arduino-ESP32 LEDC API (Core 3.x).
