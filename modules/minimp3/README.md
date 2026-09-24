# minimp3

Vendored from [lieff/minimp3](https://github.com/lieff/minimp3) (`minimp3.h` only).

CC0 1.0 (public domain). Used by the ESP32-S3 audio port to decode MPEG-1/2
Layer III into native 16 kHz stereo PCM. Do not include this from the MJX
system layer.

Local change: `MINIMP3_STATIC_SCRATCH` keeps `mp3dec_scratch_t` (~16 KiB) in
BSS instead of the calling thread stack. The implementation unit
(`firmware/platform/esp32s3/minimp3_impl.c`) defines that macro.
