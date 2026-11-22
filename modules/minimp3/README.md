# minimp3

Vendored from [lieff/minimp3](https://github.com/lieff/minimp3) (`minimp3.h` only).

CC0 1.0 (public domain). The [reference audio port](../../platform/esp32s3/audio.c) uses minimp3 for its MP3 playback path. The optional Hi-Fi media player uses [dr_mp3 in the common decoder](../../common/audio/aster_audio_decoder.c) for local tracks. Board audio settings determine the output format.

Local change: `MINIMP3_STATIC_SCRATCH` keeps `mp3dec_scratch_t` (~16 KiB) in
BSS instead of the calling thread stack. The implementation unit
[minimp3_impl.c](../../platform/esp32s3/minimp3_impl.c) defines that macro.
