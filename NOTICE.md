# Provenance and licenses

Project-owned code was refactored from the local MJX project at commit
`e235ca2f4f557163431045e2b8433e82b68d0a07` (Copyright (c) 2026 mjgame, MIT).
The original project was not modified. See LICENSE for the retained license.

Aster renames the application namespace, separates repositories, relocates dependencies,
and adds independently versioned contract/target distribution. Fixed-width MJA1/MJXI,
cpfont and file-transfer wire markers preserve their format names; Aster product/target
checks and the new API names require rebuilding old applications.

Third-party source, binary and asset license notices are retained at their locations.
Firmware includes WAMR (Apache-2.0 with LLVM exception), LVGL (MIT), minimp3 (CC0),
image codecs, pretty-poly and font resources with their own notices. The SDK includes
wasi-sdk/LLVM, WAMR compiler, Binaryen and icon assets with their respective licenses.
Only the components present in this repository are distributed by it.
