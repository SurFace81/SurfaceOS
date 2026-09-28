; The SDK runtime (abi/sdkimage.h), built from src/sdk/runtime and linked
; to run at SDK_CODE_ADDRESS. The kernel never runs these bytes: it copies
; them into the SDK code pages at boot (sdkpage.cpp).

section .rodata
align 16

global sdk_runtime_start
global sdk_runtime_end

sdk_runtime_start:
    incbin "bin/sdk/runtime.bin"
sdk_runtime_end:
