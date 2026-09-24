# Lifecycle contract fixture

hello.wasm is a precompiled Aster ABI 3.2 fixture produced from the hardware-neutral
hello example during this refactor. It logs from init and the first frame, then exits.
It is checked into the firmware repository so firmware CI needs neither an SDK
checkout nor a WASM compiler. The source equivalent is intentionally simple:
init checks the ABI and logs; event logs and calls aster_app_exit(0).
