# West-tree patches

The base patch, Waveshare QSPI correction, shared-I2C recovery, USB console
batching and opt-in PSRAM TX DMA patch, applied to `zephyr/`
(the west tree). It must be (re-)applied after any `west update` or
checkout of a different Zephyr revision.

Baseline: the Zephyr tree is kept at a stable tag (**v4.4.2**) with
**zero local commits** — every deviation lives in this folder as
patch files. Nothing is committed in the tree, so `git status` there shows
the patches as working-tree modifications and nothing else.
Never baseline on `main`: the SPI driver's busy-wait loop and
the HAL API it calls are exactly what upstream churns, so a moving
baseline means re-validating the display pipeline on every pull.

## aster_zephyr_v4.4.2.patch

One patch, eleven files, six changes: three features behind default-n
Kconfigs that ASTER's board conf enables, two unconditional bug fixes,
and one opt-in devicetree property (`mdac-alt`).

### 1. SPI sleep-wait + DMA chunk size (`drivers/spi/`)

- `CONFIG_SPI_ESP32_SLEEP_WAIT`: converts the per-DMA-chunk busy-wait
  `while (!spi_hal_usr_is_done())` into a sleep on the SPI trans_done
  interrupt. A single-core display pipeline (ASTER
  `CONFIG_ASTER_DISP_ASYNC`) cannot overlap render and transfer while
  the flusher holds the core spinning; with the wait as a sleep the
  wire time becomes free CPU for rendering. Gated on
  `!SPI_ESP32_INTERRUPT` so the ISR never runs the transfer loop.
- `CONFIG_SPI_ESP32_DMA_CHUNK_SIZE` (default 4092 = upstream
  behaviour): raises the bytes per user command; the GDMA driver
  auto-splits the buffer into its descriptor chain, cutting the
  per-4092B restart overhead. ASTER sets it to 16384.
- The chunk size is a **ceiling, not a constant**: when the
  internal-RAM bounce a non-DMA-capable source needs cannot be
  `k_malloc`'d, `spi_esp32_bounce_shrink()` halves `data->dma_chunk`
  (floor 4092 = one GDMA descriptor) and the transfer fails once.
  Trade throughput for a live panel — see change 3 for why failing
  outright is not an option. Init is in `spi_esp32_init()`; the chunk
  never grows back within a boot, and a boot that never starves the
  heap never shrinks.

Measured 2026-09-11 on the reference glass (full-frame flush):
33.5 -> 47.5 fps (async pipeline on), display clean (user-verified);
47.5 fps re-held after the v4.4.2 migration. The 2026-09-22 adaptive
change leaves that path untouched: the shrink only fires after a
failed allocation, so the desktop (heap wide open, `free=50,156` for a
16,384 B ask) still runs the full 16 KB chunk.

### 2. ST7789V CS hold + PCA idle-write skip (`drivers/display/`, `drivers/gpio/`)

Originally local commit 9aba74ed9eb (2026-09-08).

- `ST7789V_CS_ASSERT_AFTER_RESET`: parks CS low once after panel
  reset and takes CS away from the SPI driver (GPIO CS cleared,
  `slave = 0xFF` = HAL "no CS"), so pixel traffic is gated by DC
  only. `ST7789V_HOLD_CS_AFTER_INIT`: alternative for non-parked
  setups — copies `dbi_config` into RAM at init and arms
  `SPI_HOLD_ON_CS` after the first successful DISPON (the DTS
  `mipi-hold-cs` property covers init too, and this glass needs CS
  edges to latch SLPOUT/DISPON — verified black screen).
- `gpio_pca95xx.c` + `gpio_pca_series.c`: skip the I2C write when the
  expander output register is unchanged. LCD CS sits on a PCA9557, so
  every CS assert was a full I2C transaction; HOLD_ON_CS still
  re-asserts, this removes the tax. Measured 20.3 -> 24.5 fps
  full-screen before the async pipeline existed
  (docs/verification.md [1.5] #25).

### 3. The chunk loop stops on error (`drivers/spi/spi_esp32_spim.c`)

Not behind a Kconfig: this one is a hang, and it is reachable in the
stock configuration.

`spi_esp32_transfer()` advances the SPI context (`spi_context_update_tx`
/ `_rx`) only on the path that actually clocks a chunk out. **Every**
error path returns before that — most easily hit when the `k_malloc()`
that bounces a non-DMA-capable (PSRAM) chunk through internal RAM
fails — and both callers ignored the return value:

    do {
        spi_esp32_transfer(dev);
    } while (spi_esp32_transfer_ongoing(data));

so `spi_context_tx_on()` stays true forever. One failed allocation
becomes an infinite retry loop that never leaves the calling thread:
measured **142 errors/second**, paced by the console itself (~50 chars
at 115200 ≈ 7 ms per line). The symptom is a whole-device hang — panel
frozen, shell unresponsive — with **no fault**, i.e. it presents as
"wedged", not "crashed".

Reachable whenever a PSRAM-sourced transfer bigger than the free DRAM
heap is requested. Bounce size is `SPI_ESP32_DMA_CHUNK_SIZE` (16384 in
ASTER), the DRAM heap is 64 KB, and AOT app text is allocated from that
same heap — a 43 KB app leaves ~13 KB free, so a 16 KB bounce cannot
succeed. Full analysis in `docs/memory.md`; the ASTER-side trigger
(per-band scratch doubling that size) is fixed in
`firmware/runtime/aster_native.c`.

The fix captures the status and hands it to `spi_context_complete()`:
a low-memory flush now fails cleanly and LVGL retries it next refresh.
Applied to the thread-context loop (the only one compiled here —
`CONFIG_SPI_ESP32_INTERRUPT` is off in all four ASTER profiles) and to
the ISR copy of the same loop for consistency; the ISR half is not
compiled by anything ASTER builds, so it is unaudited at runtime.

**Stopping the loop was only half a fix** (2026-09-22). It turns a
hang into a dropped frame, but in the case above *every* frame is
dropped: heap starved by a large app, 16 KB bounce never fits. The
panel then keeps the last image it managed to write while the app runs
on — reported as "player won't open, then the system freezes".
`aster.player` (37 KB of AOT) hit it exactly: `free=15,636` against a
16,384 B ask, 748 B short.

The other half is the adaptive chunk in change 1. One allocation
fails, the chunk halves, and the next frame goes out at 8192 B.
Sacrificing wire throughput is the point: the 112 µs/restart that
16,384 was chosen to avoid is a rounding error next to a dead panel.

### 4. UTF-8 long file names (`subsys/fs/Kconfig.fatfs`, `modules/fatfs/`)

`CONFIG_FS_FATFS_UNICODE` sets `FF_LFN_UNICODE=2`, so long names are
exchanged with the FS API as UTF-8 instead of being converted to the
OEM code page (`CONFIG_FS_FATFS_CODEPAGE`, 437 by default).

This is not cosmetic. FatFs stores long names as UTF-16 and converts on
the way out with `put_utf()`, which in ANSI/OEM mode returns 0 for any
character the code page has no slot for. `ff.c`'s `f_readdir` path
treats a zero from `put_utf()` as "the LFN is invalid" and falls back
to the 8.3 short name; `f_open()` in the other direction re-encodes the
UTF-8 the caller passed and matches nothing. A name outside the code
page is therefore listed as garbage **and** unopenable. CJK is the case
that matters here: a card written by a Chinese Windows holds names like
`空山（…）.MP3`, whose short name is the raw GBK bytes of the original —
not text in any encoding this firmware speaks.

With UTF-8 names end to end, both directions work; the code page is
left to short names only. Verified on the bench 2026-09-21
(docs/verification.md [1.5] #48).

One interaction worth knowing, because it is not obvious from the
patch: this is also the change that makes FatFs reach its
`inaccessible object name` branch. An entry whose long name *and*
short name both fail to decode now comes back as the literal name
`?` (ff.c: `fno->fname[di++] = '\?'`), where the OEM path used to
hand back control bytes or an empty string. Anything that filters
directory entries by name has to know that placeholder — ASTER's
`aster_fs_name_valid()` does, since the bench card's root carries a
sector of such entries (docs/verification.md [1.5] #48).

**Still true after this:** a name that fits 8.3 *and* was written as
OEM bytes with no LFN (a Windows machine that had no need to make one)
is still handed over as OEM, because there is no UTF-16 to convert
from. Such a name shows as box-drawing glyphs under code page 437.
Nothing in the FS API can tell those bytes apart from Latin-1.

### 5. I2S: in-flight buffers go back to the slab (`drivers/i2s/`)

Not behind a Kconfig either: this one corrupts a kernel data structure,
and it is reachable from any `hw audio` stop, i.e. from the player's
every track change.

`i2s_esp32_{rx,tx}_stop_transfer()` set the in-flight DMA buffer's
`mem_block` to NULL and let it go. Buffers still in the queue are
returned by `i2s_esp32_queue_drop()`; the one the DMA descriptor chain
currently points at is not, so **every stop leaked a slab block** —
about three `hw audio rec` runs exhaust the slab, after which
`tx_silence()`'s allocation times out (-EAGAIN) and the caller sees
**-12**.

The obvious fix — just free it — was wrong, and the way it is wrong is
worth keeping in mind: `dma_stop()` → `gdma_ll_rx_stop()` only *writes a
stop bit*; the channel honours it at the next descriptor boundary, and
the only thing that force-resets it is `gdma_ll_rx_reset_channel()`,
which runs from `dma_config`/`dma_reload` — start-path calls. So the
descriptor is **still writing into the buffer** when the free lands.
What it writes is silence, i.e. zeros, straight over the 4-byte free-list
next pointer `k_mem_slab_free()` just stored in that block: the free list
is truncated to one element while `num_used`/`num_free` still look sane.
The next capture then pops the last block, the one after that finds
`free_list == NULL` and returns **-ENOMEM** with `num_free` still
reporting 5 → driver state ERROR → `i2s_read` switches to `K_NO_WAIT` →
empty queue → **-EIO**, returned instantly instead of blocking.

So the fix is a `dma_reload()` pulse on the same buffer *before* the
free — it drops the in-flight descriptor — plus clearing `dma_pending`
(after a reset nothing is in flight, and leaving the flag set makes
later STOPs wait for a callback that will never come). Both callbacks
also NULL `mem_block` on their error paths so a double free is not
possible.

Verified 2026-09-21 with six consecutive captures: all clean, no `FAIL`,
no `RX slab alloc failed`, and `peak` reporting real room tone
(1318/1075/1135/1877) instead of the 0 that a broken link produced.
The ASTER side that hits it is `hw audio rec` and every player track
change; both now run indefinitely.

Left alone deliberately: `i2s_esp32_configure(freq==0)` only zeroes the
config through `queue_drop()` without stopping the peripheral, and
neither `*_stop_transfer()` calls `i2s_hal_{rx,tx}_stop()`. The channel
is now reset, so nothing writes memory — that one is a power/robustness
account, not a correctness bug.

### 6. ST7789V runtime window switch (`drivers/display/`, `dts/bindings/`)

Opt-in by devicetree: a panel node that declares `mdac-alt` (the second
MADCTL value) gets a working `display_set_orientation()`, which upstream
stubs out with `-ENOTSUP` for everything but NORMAL. No Kconfig, because
the property's absence is the gate — without it the driver behaves
exactly as before, down to the error code.

The point is that the window stops being a build-time property. The
ST7789V frame memory is 240x320 and MV (0x20) transposes the counters,
so the same controller can present either window; the board used to pick
one per firmware image, via a devicetree overlay and a rebuild
(`firmware/boards/…portrait.overlay`). With both values declared, ASTER
switches at runtime and `hw display orient` exercises it.

Three things make it safe rather than merely working:

- **The alternate window is derived, never declared.** MV only
  transposes, so `mdac-alt` differing from `mdac` in MV means the same
  rectangle with its edges swapped, and agreeing in MV means the same
  edge lengths (a 180 degree turn). `width`/`height` describe one
  window; a second pair of properties is a second thing to keep in
  sync, and the first one to drift.
- **A window that cannot be addressed is refused before the command is
  sent.** `ST7789V_WINDOW_IS_LEGAL` is the same predicate in the
  BUILD_ASSERTs and in `set_orientation()`, so a DTS that pairs MV with
  the wrong geometry fails the build, and a runtime request that would
  overflow CASET/RASET gets `-EINVAL` instead of garbage in frame
  memory. (The assertion is not independent of the primary one when the
  window fills the frame memory — there, offsets are necessarily 0 and
  the two candidate windows are each other's transpose under the two MV
  states, so it cannot fire; it earns its keep on a partial window. Both
  are kept, one costs a line.) It is a macro rather than a function
  because `_Static_assert` needs a constant expression.
- **An `mdac-alt` that is not a transpose is refused, at build time
  and at runtime.** Every value of `display_orientation` is a rotation,
  and a quarter turn has to change MV; an `mdac-alt` agreeing with
  `mdac` about MV is the same rectangle mirrored. Accepting it would
  write the register, return 0, and leave the caller looking at an
  unchanged aspect -- the request answered with a success that is not
  one. The assertion catches a board that declares one (including the
  copy-paste case, where `mdac-alt == mdac`), and `set_orientation()`
  refuses it anyway, because a binding is not a contract the driver can
  assume was checked.
- **`get_capabilities()` reports the CURRENT window** (and
  `current_orientation`), not the DT's. Everything downstream sizes
  itself from it — ASTER's logical geometry, and through it the touch map
  in `platform/esp32s3/display.c`, which is indexed by the window. So
  the touch calibration follows the switch with no code of its own.

The mapping between "which orientation was asked for" and "which MADCTL
answers it" is split across two files by necessity (Zephyr's request is
relative to the DT window; the platform thinks in aspects), which is
exactly the shape that fails quietly when the two are inverted: the
panel still switches, just to the wrong window. `st7789v_window_for()`
and `platform/esp32s3/display.c:orientation_request()` are kept pure
(scalars in, scalars out) so a host harness can slice both and compose
them — see `docs/verification.md` row 84.

## Workflow

Restore a tree (after `west update` or any checkout):

    cd zephyr && git checkout v4.4.2 && west update \
        && west blobs fetch hal_espressif
    git apply ../aster/firmware/patches/aster_zephyr_v4.4.2.patch

Regenerate (from `zephyr/`, with the changes applied — scope the diff
to the eleven files, never sweep in unrelated working-tree edits):

    git diff v4.4.2 -- drivers/spi/Kconfig.esp32 drivers/spi/spi_esp32_spim.c \
        drivers/spi/spi_esp32_spim.h drivers/display/Kconfig.st7789v \
        drivers/display/display_st7789v.c drivers/gpio/gpio_pca95xx.c \
        drivers/gpio/gpio_pca_series.c modules/fatfs/zephyr_fatfs_config.h \
        subsys/fs/Kconfig.fatfs drivers/i2s/i2s_esp32.c \
        dts/bindings/display/sitronix,st7789v.yaml \
        > ../aster/firmware/patches/aster_zephyr_v4.4.2.patch

Check the patch against a tree that has it applied, without touching
either (this fails loudly if the tree drifted from the patch):

    git apply --check --reverse ../aster/firmware/patches/aster_zephyr_v4.4.2.patch

Verify presence without touching the tree (ASTER build breaks loudly
without it, via the Kconfig symbols; the I2S fix has no symbol, so it
is spotted by its comment):

    grep -q SPI_ESP32_SLEEP_WAIT drivers/spi/Kconfig.esp32 && \
    grep -q ST7789V_CS_ASSERT_AFTER_RESET drivers/display/Kconfig.st7789v && \
    grep -q FS_FATFS_UNICODE subsys/fs/Kconfig.fatfs && \
    grep -q "free-list pointer stored in the block" drivers/i2s/i2s_esp32.c && \
    grep -q mdac-alt dts/bindings/display/sitronix,st7789v.yaml

Note: the dropped DVP camera fix (local commit 5e90146023c, lckfb-era,
unused by ASTER display work) is recoverable from the zephyr repo's own
`main` branch, which is the old pre-v4.4.2 line and carries it.

The I2S fix used to be a local commit (`7250e736c27`, 2026-09-21) on
top of the tag; it was folded into this patch and the local commit
dropped, so the tree is once again exactly `v4.4.2` plus these files.
That branch — `backup/v4.4.2-aster-i2s-20260921` — is still there if the
commit is ever wanted back.

## aster_zephyr_v4.4.2_qspi.patch

Apply after the base patch for Waveshare AXS15231B. `SPI_LINES_QUAD` lives
in operation bits 16..17, but `spi_esp32_get_line_mode(uint16_t operation)`
truncated these bits and selected single-lane transfers even with
CONFIG_SPI_EXTENDED_MODES. Use `spi_operation_t`, which is 32 bits when
extended modes are enabled and remains 16 bits otherwise. This does not
change the single-lane LCKFB configuration. Apply with:

```bash
git -C /path/to/zephyr apply /path/to/aster-firmware/patches/aster_zephyr_v4.4.2_qspi.patch
```

## aster_zephyr_v4.4.2_psram_dma.patch

Apply after the base and QSPI patches. `CONFIG_SPI_ESP32_TX_PSRAM_DIRECT`
(default n, ESP32-S3 + PSRAM + DMA only) avoids internal-RAM bounce buffers
for TX buffers whose address and length are multiples of 64 bytes.
CPU cache is written back before GDMA; a failed writeback aborts the send.
Unaligned buffers and RX retain the original paths.

External TX descriptor payloads are at most 4032 bytes (4092 rounded down
to a 64-byte boundary), so every next descriptor remains aligned. Aligned
PSRAM transfers select a 64-byte external memory block; other TX selects 16.
Waveshare currently DISABLES this option: physical scrolling tests showed
horizontal/rectangular artifacts with direct PSRAM TX, which disappeared
with internal-RAM bounce + GDMA. Completion fencing alone did not fix it.
The experimental patch is retained for diagnosis, not as the board default;
alignment/cache-writeback checks alone do not establish physical stability.

```bash
git -C /path/to/zephyr apply /path/to/aster-firmware/patches/aster_zephyr_v4.4.2_psram_dma.patch
```

## aster_zephyr_v4.4.2_usb_serial_console_batch.patch

Batch USB Serial/JTAG console output. `serial_esp32_usb_poll_out` flushed
(`WR_DONE`) after every single character, and log lines reach it one byte at a
time: with `CONFIG_LOG_MODE_IMMEDIATE` the shell log backend re-enables its
transport in blocking mode (`z_shell_log_backend_enable`), whose write path is
`polling_write` - a `uart_poll_out` per character. So every byte paid the
endpoint turnaround between a flush and the FIFO accepting the next byte -
measured on hardware at ~80us, i.e. 6.5ms for a 75-character log line and 11ms
for a 140-character one (adjacent-line timestamps in captured logs). With
immediate mode that stall lands on whatever thread logged; in the cube app a
burst of six timing lines every 64 frames (30ms+ for the burst, adjacent
timestamps) was long enough to miss the frame deadline.

Bytes now accumulate in the 64-byte TX FIFO, which the hardware flushes on its
own when full:

- a `'\n'` flushes synchronously (every log line ends in one, so lines - panic
  output included - reach the host as before), and so does any byte logged
  from an ISR;
- any other byte (shell prompts, character echo) is flushed by a 250us one-shot
  timer, so typing still echoes immediately;
- when the timer finds the FIFO still busy (it filled exactly to 64 bytes and
  auto-flushed), it retries briefly, because a 64-byte packet is an incomplete
  USB transaction until more data or a zero-length packet follows (see the
  flush note in `usb_serial_jtag_ll.h`); the retry is bounded so a disconnected
  host cannot leave a timer spinning.

Measured after (waveshare-35b, cube app running, usb-debug image): the same
six-line burst costs 4ms wall (~0.7ms/line, all six timestamps in the same
millisecond), multi-line shell output (`aster app list`) arrives intact, and
prompt/echo latency is unchanged (the 250us timer covers newline-less text).
The companion firmware change (ui_gfx timing dump moved to a worker thread) is
what actually keeps the burst off the render thread; this patch is what makes
each line cheap for whichever thread emits it.

The change is unconditional but self-contained to this driver; `fifo_fill`
(the interrupt-driven path) already batched its flush and is untouched. Apply
after the other west-tree patches:

```sh
git -C ../../zephyr apply "$PWD/patches/aster_zephyr_v4.4.2_usb_serial_console_batch.patch"
```

## aster_zephyr_v4.4.2_sram_tail_heap.patch

On ESP32 with the libc heap in the SRAM tail, place the kernel heap at the
front of that tail instead of a `.bss` array, and allocate the system
workqueue and shell stacks from that tail when each thread is created.
Apply to the Zephyr tree:

```sh
git -C ../../zephyr apply "$PWD/patches/aster_zephyr_v4.4.2_sram_tail_heap.patch"
```

## aster_zephyr_v4.4.2_esp_task_stacks.patch

Allocate the ESP timer task and the ESP32-S3 Wi-Fi task stack when the task
is created, from the same internal tail, instead of keeping them in `.bss`.
Apply to `modules/hal/espressif`:

```sh
git -C ../../modules/hal/espressif apply "$PWD/patches/aster_zephyr_v4.4.2_esp_task_stacks.patch"
```


## aster_zephyr_v4.4.2_tx_prefetch.patch

Apply after the existing base, QSPI and PSRAM-DMA patches. Adds internal
clock diagnostics and default-off `SPI_ESP32_TX_PREFETCH` (S3, sleep-wait DMA).
Per-device enabling remains explicit. Only contiguous, TX-only, non-DMA-capable
sources at least two bands long qualify. Two internal 8 KiB bands share the
previous 16 KiB chunk budget; an adaptively reduced 8 KiB ceiling uses two
4 KiB bands, while the 4092 B fallback retains the legacy path. Allocation
failure falls back before sending. Current DMA source stays immutable while
the next source band is copied; buffers free only after completion/error
cleanup, under the existing SPI context ownership. RX/short/fragmented transfers
remain on the previous implementation. Physical tests and tuning results are
recorded separately; this patch does not enable PSRAM direct DMA.

Optional `SPI_ESP32_TX_PREFETCH_LARGE_BANDS` doubles the pair budget: at the
16 KiB DMA ceiling, two 16 KiB bands allocate 32 KiB. Each DMA command remains
at most 16 KiB. A reduced 8 KiB ceiling uses two 8 KiB bands; 4092 B keeps the
legacy path. The option defaults off and needs an explicitly increased internal
heap and hardware verification. This is additional memory, not a free increase.
