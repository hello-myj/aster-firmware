# Lifecycle contract fixture

hello.wasm is the precompiled ABI 4.2 lifecycle fixture from hello.c in this directory.
It checks the ABI, logs from the first frame, then exits.
It is checked into the firmware repository so firmware CI needs neither an SDK
checkout nor a WASM compiler. The source equivalent is intentionally simple:
init checks the ABI and logs; event logs and calls aster_app_exit(0).

The fixture checks its compiled ABI against the host runner's firmware ABI
-- so it is stale after every ABI bump and the lifecycle
test fails (init returns ASTER_EABI) until it is regenerated:

    clang --target=wasm32-unknown-unknown -O1 -nostdlib -ffreestanding \
      -I contract/include tests/fixtures/hello.c -Wl,--no-entry \
      -Wl,--export=aster_app_init -Wl,--export=aster_app_start \
      -Wl,--export=aster_app_event -Wl,--export=aster_app_stop \
      -Wl,--export=aster_app_destroy -o tests/fixtures/hello.wasm
