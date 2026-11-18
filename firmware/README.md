# PillMate voice reminder firmware

## What is included

- `PillMate_Box_Controller_voice_transfer.ino` is based on the existing ESP32-S3 controller.
- The local setup page accepts one WAV reminder per drawer.
- Uploads are checked for PCM, mono, 16 kHz, 16-bit WAV and stored as `/voice1.wav` through `/voice4.wav` in LittleFS.
- The dashboard now converts browser recordings to that WAV format and saves them in Supabase. The saved recording list includes a download link for transfer to the device.

## Flash storage partition

The ESP32-S3-N16R8 uses 16 MB flash. In Arduino IDE, install/select a custom partition scheme based on `partitions_pillmate_16mb.csv` (6 MB application, remaining space for LittleFS). The filesystem partition label must be `spiffs`, because Arduino-ESP32's LittleFS mount uses that label by default.

The firmware intentionally mounts LittleFS with formatting disabled. It will not automatically format or erase existing device storage.

## Transfer workflow

1. Record and save a voice reminder on the dashboard.
2. Download its WAV file.
3. Connect the phone or laptop to `PillMate-S3`.
4. Open `http://192.168.4.1`.
5. Choose the drawer and WAV file in the Voice Reminder Audio section, then upload.

## Playback status

The upload and local storage path is implemented. Scheduled speaker playback is not yet enabled in this source. The supplied controller source assigns GPIO 4 to Drawer 4's servo and does not define an I2S/DAC audio interface. Before adding playback, confirm the actual audio output interface and its signal pin(s); do not reuse a servo pin or guess I2S wiring.
