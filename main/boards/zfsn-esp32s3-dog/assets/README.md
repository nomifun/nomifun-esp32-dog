dog_assets.bin is this board's prebuilt assets-partition image. `idf.py flash`
writes it to the `assets` partition at offset 0xc20000.

Size: 624371 bytes
SHA256: b3e37000e6eef19864e32dccfb7f7be286f1901a1ca4a154870d77be1d4e3d9e

Contents, per the image's own index.json:
- text font: font_puhui_common_14_1
- 21 PNG emotions: angry, confident, confused, cool, crying, delicious,
  embarrassed, funny, happy, kissy, laughing, loving, neutral, relaxed, sad,
  shocked, silly, sleepy, surprised, thinking, winking
- ESP-SR wake-word model wn9_nihaoxiaozhi_tts ("你好小智"), packed as
  srmodels.bin

These third-party binary resources keep their own licenses and are not
covered by this repository's MIT license.
