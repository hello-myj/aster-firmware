# dr_libs

Upstream: https://github.com/mackron/dr_libs
Commit: f12e197fd462e4c22873fbd70d11b1d2f2c1dc3c
License: MIT-0 (license text retained at the end of each header).
Unmodified upstream headers. Used with custom Aster OS I/O/allocation callbacks, no stdio.

- `dr_flac.h` SHA256 `3f010d3142a5b9df80a0bd8b325de54317468b1aff85abb257c86dbfefe9bd2a`
- `dr_wav.h` SHA256 `7c9660d75361ad6175e6217c63a1075c9512445c06bc951197fc479179f76b39`
- `dr_mp3.h` SHA256 `7a6cc38b0afdb55cfc9eec991d9169d67beb8fd910be2995121510cdaa99c356`

The optional [local media decoder](../../common/audio/aster_audio_decoder.c) uses dr_wav, dr_flac and dr_mp3 for SD-card playback. It owns file I/O, tracks decoder allocations and builds a seek index for MP3. The IDF audio component includes these sources when the board declares `ASTER_BOARD_MEDIA_PLAYER`. See the [FAKE POD Hi-Fi guide](../../docs/fakepod-s31-hifi-design.zh-CN.md).
