# Lifecycle contract fixture

hello.wasm is a precompiled Aster ABI 3.3 fixture produced from the hardware-neutral
hello example during this refactor. It logs from init and the first frame, then exits.
It is checked into the firmware repository so firmware CI needs neither an SDK
checkout nor a WASM compiler. The source equivalent is intentionally simple:
init checks the ABI and logs; event logs and calls aster_app_exit(0).

The fixture reports the ABI it was compiled against, and the host runner compares it
against the firmware's own -- so it is stale after every ABI bump and the lifecycle
test fails (init returns ASTER_EABI) until it is regenerated:

    python tools/aster.py build apps/hello --target lckfb-s3 --no-aot   # in aster-sdk
    cp build/lckfb-s3/aster.hello/app.wasm <firmware>/tests/fixtures/hello.wasm
