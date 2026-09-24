# ASTER App Package Format `.mja` (MJA1)

A `.mja` file is a single-container ASTER application package:

```
+---------------------------+
| Header (64 or 72 bytes, LE)|
+---------------------------+
| manifest (JSON, UTF-8)    |
+---------------------------+
| app.wasm  (universal)     |  required
+---------------------------+
| app.aot   (optional)      |  platform-optimised
+---------------------------+
| icon      (optional)      |  launcher tile (not UI widgets)
+---------------------------+
| assets    (optional MJR1) |  app bitmaps; hdr_size=72
+---------------------------+
```

All blobs are 4-byte aligned (padding between sections is allowed;
offsets point at the first byte of each blob).

## Header (64 bytes, or 72 with assets, little-endian)

| off | size | field |
|---|---|---|
| 0 | 4 | magic `"MJA1"` |
| 4 | 2 | format version (=1) |
| 6 | 2 | header size (64 without assets, **72** when MJR1 is present) |
| 8 | 4 | flags (bit0 = SIGNED, bit1 **reserved** (was HAS_UI), bit2 = HAS_ASSETS, others 0) |
| 12 | 4 | manifest offset |
| 16 | 4 | manifest length (bytes, JSON) |
| 20 | 4 | wasm offset |
| 24 | 4 | wasm length |
| 28 | 4 | aot offset (0 = absent) |
| 32 | 4 | aot length |
| 36 | 4 | aot target id (`ASTER_AOT_TARGET_*`, 0 if absent) |
| 40 | 4 | icon offset (0 = absent) |
| 44 | 4 | icon length |
| 48 | 4 | **reserved, must be 0** — was meant to be icon format (`ASTER_ICON_*`), never used |
| 52 | 2 | icon width (**always 0 today**, see below) |
| 54 | 2 | icon height (**always 0 today**, see below) |
| 56 | 8 | **reserved, must be 0** (was ui offset/length, UIB1 removed in ABI 3.0) |

> **The four icon fields at 48..55 carry no information.** `pack_mja.py` never writes
> offset 48 (it packs only `icon_off`/`icon_len` at 40/44 and w/h at 52/54, and the
> w/h arguments are never passed, so they stay 0); the firmware never reads offset 48,
> and `aster_mja.c` reads 52/54 into `aster_manifest_t.icon_w/icon_h` — which **no consumer
> uses**. The real icon format lives inside the MJXI blob at its own offset 10; the
> real dimensions live there too (offsets 6/8).
>
> Do not start relying on 52/54: a packer that fills them would be writing a field
> the reader ignores, and the reader trusting them would be trusting a field the
> packer leaves at 0. Either wire both ends or leave the whole 48..55 span reserved.
> Validating an icon means calling `aster_icon_parse()` on the blob.
| 64 | 4 | assets offset (only if hdr_size=72; 0 = absent) |
| 68 | 4 | assets length |

Format version stays **1**. Old 64-byte headers remain valid. The parser accepts `hdr_size == 64 || hdr_size == 72`.

The 8-byte extension is **not** a new MJA version: packages without UI resources still use a 64-byte header so existing unsigned/signed images verify unchanged.

## MJR1 asset blob

When `assets_len != 0` the blob at `assets_off` is:

```
16B header: magic "MJR1", ver=1, file_count u16, reserved 8
then file_count × 12B: path_off, data_off, data_len  (offsets from blob start)
then path strings (NUL-terminated) and file bytes, 4-byte padded
```

Paths are relative (`icons/clock.bin`), no `..`, no leading `/`. Install extracts to `/SD:/aster/apps/<id>/assets/<path>`. Reinstall deletes the old `assets/` tree first and keeps `data/`. The original `package.mja` is still stored in full (same double-copy as wasm).

App icons use the **MJXI** pixel format (see [gfx-engine.md](gfx-engine.md)). No fonts are
packed: since ABI 2.1 every glyph streams from one TTF on the SD card
(`S:/aster/fonts/ui.ttf`) through LVGL tiny_ttf, so the `.mja` never carries a font. The
firmware does not decode PNG.

AOT target ids: 1 = xtensa-esp32s3, 2 = riscv, 3 = thumb-v7, 4 = cortex-m,
5 = x86-64. The loader picks the AOT section only when the target matches
the device AND the wasm hash recorded in the manifest is unchanged
(`wasm_sha256` first 4 bytes are embedded in the AOT filename check);
otherwise it falls back to `app.wasm` (interpreter). AOT is a cache, never
the only payload (design doc sec.38).

## Manifest JSON (schema v1)

Real, as-packed manifest for the in-tree `sdk/apps/gfxshow` (keys appear in this
order because `build.py` inserts the conditional ones only when set):

```json
{
  "id": "aster.gfxshow",
  "name": "Gfx Show",
  "version": "0.1.0",
  "abi": { "min": "3.0", "target": "3.0.0" },
  "memory": { "initial": 393216, "stack": 8192 },
  "requires": ["display", "touch"],
  "permissions": ["fs", "sd"],
  "category": "game"
}
```

**Key contract** (`build.py::manifest_for` writes; `aster_mja.c` reads):

| key | written | read by fw | notes |
|---|---|---|---|
| `id` | always | **hard-required** | reverse-DNS, immutable; install dir derives from it |
| `name` | always | **hard-required** | missing → `ASTER_ECORRUPT` |
| `version` | always | yes | free-form string, shown in `aster app list` |
| `abi` | always | yes | `{min, target}`; `min` is the install gate |
| `memory` | always | yes | `{initial, stack}`; `max` only if non-zero, `heap` only if set |
| `entry` | if set | no | `app.wasm` when present |
| `requires` / `optional` | if non-empty | yes | capability names, checked at install |
| `permissions` | if non-empty | yes | `fs` / `kv` / `appmgr` / `sd`（ABI 1.2 起硬执法） |
| `category` | if set | yes | `game` / `tool` / `system`, copied to the on-disk manifest |
| `orientation` | if set | yes | `portrait` / `landscape`, copied to the on-disk manifest |

`orientation` is the app saying which way it was laid out. **Absent means "follow
the panel"**, and absence is the only way to say it: there is no `any` spelling,
so no typo can silently become a request. A value that is neither of the two
spellings is **dropped with a warning on the device** (`aster_mja_norm_orientation`
returns `ASTER_ORIENT_ANY`), and a dropped key is indistinguishable from one that
was never there — the app follows the panel instead of failing. That is why
`verify_mja.py` asserts the spelling at package time, where the typo is still
visible, rather than letting the install be quietly forgiving.

The vocabulary is deliberately shared with the display ABI rather than copied:
the two spellings normalize to `ASTER_ORIENT_PORTRAIT` / `ASTER_ORIENT_LANDSCAPE`
(`abi/include/aster/display.h`), and both the SDK and the verifier take their
whitelist from one place (`aster_board.ORIENTATIONS`).

There is **no `format` key** — the container version lives in the header
(offset 4), and the old `"format": 1` / `"wasm_sha256"` / `"aot_sha256"` /
`"icon"` examples were never emitted by `build.py`. AOT/vendor hash checking is
done by filename, not by a manifest field (see the AOT paragraph above).

`memory.stack` = wasm operand stack bytes, `memory.heap` = wasm gc heap
(for interpreter), `memory.max` = linear memory cap enforced by the Runtime.
`storage.app` 只是 `requires` 里 `filesystem` 的别名，不是权限令牌。

## Install layout (device)

```
/SD:/aster/apps/<id>/
  ├── package.mja        (original copy)
  ├── app.wasm           (extracted)
  ├── app.aot-<target>   (extracted, optional)
  ├── icon.bin           (extracted MJXI launcher tile, optional)
  ├── manifest.json      (extracted; includes category / orientation when set)
  ├── assets/            (optional)
  └── data/              (app sandbox root, created on first write)
```

`app.toml` may set `icon = "icon.bin"` (MJXI packed into the `.mja` icon section) and `category = "game"|"tool"|"system"`, and `orientation = "portrait"|"landscape"`. Install writes the tile to `icon.bin` and copies `category` and `orientation` into the on-disk manifest. The desktop lists apps from the registry and loads `"<id>/icon.bin"`; it does not guess icons from the id string.

`aster app install <path.mja>` verifies: magic, header consistency
(offsets + lengths inside file), manifest JSON required keys, ABI compat
(`min <= device abi <= major`), `requires` capabilities present, wasm magic
`\0asm`, and the **signature** (see below). `pack_mja.py` and the firmware
parser share this layout.

## Signature (optional, ECDSA-P256 / SHA-256)

A package is signed when `header.flags` bit0 (`ASTER_FLAG_SIGNED`) is set. Then
the **last 64 bytes of the file** are a raw `r‖s` ECDSA-P256 signature. The
signed region is every byte BEFORE that block — `[0, file_size - 64)`, which
includes the header (with the flag already set), manifest, wasm, aot, icon and assets.
Placing the block at the tail means nothing inside the hashed span points at
its own location, so there is no self-referential field to exclude.

- **Trust anchor**: `CONFIG_ASTER_VENDOR_PUBKEY` — a 65-byte uncompressed P-256
  point (hex string, `04‖X‖Y`). The default in-tree key is the *development*
  pair in `sdk/keys/vendor_dev.pem` (private key committed on purpose, like
  MCUBOOT's test keys, so a clean checkout produces verifiable packages);
  production images must override it.
- **Pack**: `pack_mja.py --sign sdk/keys/vendor_dev.pem` (or the `ASTER_SIGN_KEY`
  env var consumed by `build.py`). `verify_mja.py` re-checks the signature.
- **Device policy** (`CONFIG_ASTER_SIGNATURE_POLICY`, three states):
  | policy | unsigned pkg | valid signature | bad/alien/tampered |
  |---|---|---|---|
  | `off` (448 KB fw) | install | not checked | not checked |
  | `allow-unsigned` (default, 516 KB fw) | install | install | reject `-17 ESIGN` |
  | `require` | reject `-18 EUNSIGNED` | install | reject `-17 ESIGN` |
  `off` links out all of PSA (zero cost); the other two add ~68 KB. The gate
  runs in `aster_mja_install_file` before any byte is written to the app dir;
  boot-time scanning of already-installed apps does not re-verify (once on
  disk, the install gate already passed). A `ASTER_ENOTSUP`-free PSA entropy
  source (TRNG) is required — selected automatically via Kconfig.
