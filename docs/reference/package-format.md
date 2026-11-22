# ASTER App Package Format `.ast` (ASTP)

A `.ast` file is a single-container ASTER application package:

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
| assets    (optional ASTR) |  app bitmaps; hdr_size=72
+---------------------------+
```

All blobs are 4-byte aligned (padding between sections is allowed;
offsets point at the first byte of each blob).

## Header (64 bytes, or 72 with assets, little-endian)

| off | size | field |
|---|---|---|
| 0 | 4 | magic `"ASTP"` |
| 4 | 2 | format version (=1) |
| 6 | 2 | header size (64 without assets, **72** when ASTR is present) |
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

| 64 | 4 | assets offset (only if hdr_size=72; 0 = absent) |
| 68 | 4 | assets length |

Read icon dimensions and format from the ASTI blob with `aster_icon_parse()`. The SDK packer leaves header bytes 48–55 zero.

Format version stays **1**. Old 64-byte headers remain valid. The parser accepts `hdr_size == 64 || hdr_size == 72`.

The 8-byte extension is **not** a new format version: packages without UI resources still use a 64-byte header so existing unsigned/signed images verify unchanged.

## ASTR asset blob

When `assets_len != 0` the blob at `assets_off` is:

```
16B header: magic "ASTR", ver=1, file_count u16, reserved 8
then file_count × 12B: path_off, data_off, data_len  (offsets from blob start)
then path strings (NUL-terminated) and file bytes, 4-byte padded
```

Paths are relative (`icons/clock.bin`), no `..`, no leading `/`. Install extracts into the inactive code slot of `/SD:/aster/apps/<id>/`; APPs access the selected resources through `/assets/<path>`. Upgrade keeps the current code slot intact until commit and preserves the stable `data/` and `cache/` directories. The original `package.ast` is still stored in full (same double-copy as wasm).

The launcher reads ASTI icons. The system text renderer loads the selected `ui_<px>.cpfont` from SD. An app can also bring its own font data as an asset, as Aster Music does for lyric lettering. PNG decoding is available through the image decoder. [System font faces](../../resources/fonts/faces.json) · [Image decoder](../../modules/imgdec)

The format defines AOT IDs 1 (Xtensa ESP32-S3), 2 (RISC-V), 3 (Thumb v7), 4 (Cortex-M) and 5 (x86-64). Current Aster board builds use 1 or 2. The device requires a compatible AOT section. The desktop simulator executes the WASM section and can open a package built with `--no-aot`. [Firmware package decoder](../../common/package/aster_pkg_decode.c) · [Installer](../../systems/zephyr/services/aster_pkg.c)

## Manifest JSON (schema v1)

Optional `description` is the app-owned launcher introduction: a single-line
UTF-8 string of at most 127 bytes. The installer preserves it in the normalized
manifest and FLUX reads it for every installed app, including dynamically added
IDs. Missing or empty descriptions leave the introduction blank. Quote and
backslash characters are escaped when writing the normalized JSON. This metadata
addition does not change the WASM ABI or package format version.

Selected fields from the published RISC-V Store 0.3.3 package:

```json
{
  "id": "aster.store",
  "name": "Aster 商城",
  "version": "0.3.3",
  "product": "aster",
  "target_id": "riscv",
  "abi": {
    "min": "4.3",
    "target": "4.3.0"
  },
  "requires": [
    "display",
    "touch",
    "filesystem",
    "http"
  ],
  "permissions": [
    "net",
    "fs",
    "kv",
    "appmgr"
  ]
}
```

**Manifest fields.** The SDK builds and packs the JSON. The device [package decoder](../../common/package/aster_pkg_decode.c) reads fields needed for installation and launch.

| key | written | read by fw | notes |
|---|---|---|---|
| `product` / `target_id` | always | required | `aster` and the selected CPU target, checked during package decoding |
| `id` | always | **hard-required** | reverse-DNS, immutable; install dir derives from it |
| `name` | always | **hard-required** | missing → `ASTER_ECORRUPT` |
| `version` | always | yes | free-form string, shown in `aster app list` |
| `abi` | always | yes | `{min, target}`; `min` is the install gate |
| `memory` | always | yes | `{initial, stack}`; `max` only if non-zero, `heap` only if set |
| `entry` | if set | no | `app.wasm` when present |
| `requires` / `optional` | if non-empty | yes | required capabilities are checked at launch, optional capabilities are advisory |
| `supported_boards` | if non-empty | yes | up to 8 case-sensitive board IDs (1–39 ASCII letters, digits, dots, hyphens or underscores). Any matching ID allows launch. Omitted or empty allows any compatible board |
| `permissions` | if non-empty | yes | `fs` / `kv` / `appmgr` / `sd`（ABI 1.2 起硬执法） |
| `category` | if set | yes | `game` / `tool` / `system`, copied to the on-disk manifest |
| `orientation` | if set | yes | `portrait` / `landscape`, copied to the on-disk manifest |

`orientation` is the app saying which way it was laid out. **Absent means "follow
the panel"**, and absence is the only way to say it: there is no `any` spelling,
so no typo can silently become a request. A value that is neither of the two
spellings is **dropped with a warning on the device** (`aster_pkg_norm_orientation`
returns `ASTER_ORIENT_ANY`), and a dropped key is indistinguishable from one that
was never there — the app follows the panel instead of failing. That is why
`verify_ast.py` asserts the spelling at package time, where the typo is still
visible, rather than letting the install be quietly forgiving.

The vocabulary is deliberately shared with the display ABI rather than copied:
the two spellings normalize to `ASTER_ORIENT_PORTRAIT` / `ASTER_ORIENT_LANDSCAPE`
(`contract/include/aster/display.h`), and both the SDK and the verifier take their
whitelist from one place (`aster_board.ORIENTATIONS`).

The container version is at header offset 4. The SDK packer adds `wasm_sha256` and, for native packages, `aot_sha256` and `aot_target`. The SDK verifier compares the hashes with the corresponding bytes. The device separately checks its package policy, CPU target and ABI. [SDK packer](https://github.com/hello-myj/aster-sdk/blob/main/tools/pack_ast.py) · [SDK verifier](https://github.com/hello-myj/aster-sdk/blob/main/tools/verify_ast.py)

The `memory` object carries the application's initial and maximum linear memory, stack and heap budgets in bytes. The runtime also applies local device limits.

## Install layout (device)

The device stores program files and resources in two private version slots,
with separate commit records and stable user-data directories. Existing flat
installations remain readable. The current layout, commit rules, error handling
and reboot recovery are documented in [APP installation and recovery](../package-installation.md).

`app.toml` may declare `icon`, `category` and `orientation`. Install extracts the
ASTI icon and normalized metadata into the selected code generation; the desktop
reads them through the registry. APPs keep using the public `/assets`, `/data`
and `/cache` paths, without embedding physical version-slot paths.

`aster app install <path.ast>` verifies: magic, header consistency
(offsets + lengths inside file), manifest JSON required keys, ABI compat
(equal ABI major and device minor at least the declared minimum minor), wasm magic
`\0asm`, and the **signature** (see below). `pack_ast.py` and the firmware
parser share this layout.

## Signature (optional, ECDSA-P256 / SHA-256)

A package is signed when `header.flags` bit0 (`ASTER_FLAG_SIGNED`) is set. Then
the **last 64 bytes of the file** are a raw `r‖s` ECDSA-P256 signature. The
signed region is every byte BEFORE that block — `[0, file_size - 64)`, which
includes the header (with the flag already set), manifest, wasm, aot, icon and assets.
Placing the block at the tail means nothing inside the hashed span points at
its own location, so there is no self-referential field to exclude.

The firmware verification key is configured through `CONFIG_ASTER_VENDOR_PUBKEY`. The SDK packer accepts `--sign PATH_TO_PRIVATE_PEM`; a normal SDK build reads the optional `ASTER_SIGN_KEY` environment variable. The public verification key belongs in the firmware configuration. The corresponding private key stays with the publisher.

With signature verification disabled, the signature is not checked. A build that allows unsigned packages still verifies any signature supplied. A build that requires signatures rejects an unsigned package with `ASTER_EUNSIGNED`; a failed signature check returns `ASTER_ESIGN`. Choose the policy for the specific board build. [Signature definitions](../../systems/zephyr/services/aster_sig.h)
