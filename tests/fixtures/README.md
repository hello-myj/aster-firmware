# Lifecycle contract fixture

hello.wasm is a precompiled Aster ABI 3.7 fixture produced from the hardware-neutral
hello example during this refactor. It logs from init and the first frame, then exits.
It is checked into the firmware repository so firmware CI needs neither an SDK
checkout nor a WASM compiler. The source equivalent is intentionally simple:
init checks the ABI and logs; event logs and calls aster_app_exit(0).

The fixture checks its compiled ABI against the host runner's firmware ABI
-- so it is stale after every ABI bump and the lifecycle
test fails (init returns ASTER_EABI) until it is regenerated:

    python tools/aster.py build apps/hello --target xtensa --no-aot   # in aster-sdk
    cp apps/hello/build/xtensa/aster.hello/app.wasm <firmware>/tests/fixtures/hello.wasm
