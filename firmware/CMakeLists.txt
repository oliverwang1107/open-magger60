cmake_minimum_required(VERSION 3.20)
project(magger60_fw C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # startup.c uses GNU range designators

set(KM_WARN -Wall -Wextra -Werror -pedantic -Wshadow -Wconversion -Wno-sign-conversion)

# ---- portable libraries (no hardware dependency) ----
file(GLOB KM_CORE_SRC CONFIGURE_DEPENDS core/src/*.c)
add_library(km_core STATIC ${KM_CORE_SRC} core/gen/km_slot_data.c)
target_include_directories(km_core PUBLIC core/include)
target_compile_options(km_core PRIVATE ${KM_WARN})

# The adaptive engine filters (FILTER_NOTES.md) are compiled into the core library, so the images that use them link this twin, built with the define.
add_library(km_core_f STATIC ${KM_CORE_SRC} core/gen/km_slot_data.c)
target_include_directories(km_core_f PUBLIC core/include)
target_compile_options(km_core_f PRIVATE ${KM_WARN})
target_compile_definitions(km_core_f PUBLIC KM_ENGINE_FILTER_MODES=1)

add_library(km_hid STATIC hid/src/km_hid.c hid/src/km_vendor.c hid/src/km_scope.c hid/src/km_config.c hid/src/km_stream.c)
target_include_directories(km_hid PUBLIC hid/include)
target_compile_options(km_hid PRIVATE ${KM_WARN})

add_library(km_usb STATIC usb/src/km_usb_desc.c usb/src/km_usb_ctrl.c usb/src/km_usb_in.c)
target_include_directories(km_usb PUBLIC usb/include)
target_compile_options(km_usb PRIVATE ${KM_WARN})

# The USB descriptors are compiled INTO this library, so a per-image define on the executable never reaches them (that is how the first
# km_m6h shipped bInterval 4 / bcdDevice 1). The 8 kHz image links this twin, built with the define; tools/check_image.py --usb-8khz /
# --usb-1khz reads the descriptor bytes back out of the finished .bin and fails on a mismatch.
add_library(km_usb_8k STATIC usb/src/km_usb_desc.c usb/src/km_usb_ctrl.c usb/src/km_usb_in.c)
target_include_directories(km_usb_8k PUBLIC usb/include)
target_compile_options(km_usb_8k PRIVATE ${KM_WARN})
target_compile_definitions(km_usb_8k PUBLIC KM_USB_HS_8KHZ=1)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    # ---- cross build: smoke image for the Cortex-M4F target ----
    # pedantic would reject the GNU range designators in startup.c, so it only builds with -Wall -Wextra.
    add_executable(km_smoke.elf port/n32h48x/startup.c port/n32h48x/smoke_main.c)
    target_link_libraries(km_smoke.elf PRIVATE km_core km_hid)
    target_compile_options(km_smoke.elf PRIVATE -Wall -Wextra -Werror -Os)
    target_compile_options(km_core PRIVATE -Os)
    target_compile_options(km_core_f PRIVATE -Os)
    target_compile_options(km_hid PRIVATE -Os)
    # M2-C rescue drill: valid but dead app (see port/n32h48x/crash_main.c); not part of the product
    add_executable(km_crashtest.elf port/n32h48x/startup.c port/n32h48x/crash_main.c)
    target_compile_options(km_crashtest.elf PRIVATE -Wall -Wextra -Werror -Os)
    # D1 "alive" image: our own startup (VTOR, FPU, IWDG feed) and nothing else
    add_executable(km_d1.elf port/n32h48x/app_startup.c port/n32h48x/d1_main.c)
    target_compile_options(km_d1.elf PRIVATE -Wall -Wextra -Werror -Os)
    # D2: USB enumeration only
    add_executable(km_d2.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/d2_main.c)
    target_link_libraries(km_d2.elf PRIVATE km_usb)
    target_compile_options(km_d2.elf PRIVATE -Wall -Wextra -Werror -Os)
    # D3: D2 plus the vendor channel (cmd 3 only)
    add_executable(km_d3.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/d3_main.c)
    target_link_libraries(km_d3.elf PRIVATE km_usb km_hid)
    target_compile_options(km_d3.elf PRIVATE -Wall -Wextra -Werror -Os)
    # D4: D3 plus enter-bootloader from the host (the only flash write: the boot flag)
    add_executable(km_d4.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/d4_main.c)
    target_link_libraries(km_d4.elf PRIVATE km_usb km_hid)
    target_include_directories(km_d4.elf PRIVATE core/include)
    target_compile_options(km_d4.elf PRIVATE -Wall -Wextra -Werror -Os)
    # K1 (M4_PLAN.md): D4 with our own USB descriptors and the key-report IN path, no scanning, no keys sent by default
    add_executable(km_k1.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/k1_main.c)
    target_link_libraries(km_k1.elf PRIVATE km_usb km_hid)
    target_include_directories(km_k1.elf PRIVATE core/include)
    target_compile_options(km_k1.elf PRIVATE -Wall -Wextra -Werror -Os)
    # M3 sensing driver (sense_*.c, adc_scan.c): register-level code, see SENSING_BRINGUP_NOTES.md
    add_library(km_sense STATIC port/n32h48x/sense_tables.c port/n32h48x/sense_hw.c port/n32h48x/adc_scan.c)
    target_include_directories(km_sense PUBLIC port/n32h48x)
    target_compile_options(km_sense PRIVATE ${KM_WARN} -Os)
    # S1: polled single conversions (ESC, left CTRL), S2: polled scan of all 76 slots, S3: GTIM1 + ADC + DMA full scan
    foreach(S s1 s2 s3)
        add_executable(km_${S}.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/${S}_main.c)
        target_link_libraries(km_${S}.elf PRIVATE km_sense km_core km_usb km_hid)
        target_include_directories(km_${S}.elf PRIVATE core/include)
        target_compile_options(km_${S}.elf PRIVATE -Wall -Wextra -Werror -Os)
    endforeach()
    # M4 K2 / K3: scan + key engine + keyboard reports. K2 reports only ESC (safety gate), K3 every key.
    foreach(V k2 k3)
        add_executable(km_${V}.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/m4_main.c)
        target_link_libraries(km_${V}.elf PRIVATE km_sense km_core km_usb km_hid)
        target_include_directories(km_${V}.elf PRIVATE core/include)
        target_compile_options(km_${V}.elf PRIVATE -Wall -Wextra -Werror -Os)
    endforeach()
    # M5 T-D: BUILD-ONLY probe of the settings store on the real flash driver (never flashed in T-D; for the address audit)
    add_executable(km_store_probe.elf port/n32h48x/app_startup.c port/n32h48x/store_flash.c port/n32h48x/store_probe_main.c)
    target_link_libraries(km_store_probe.elf PRIVATE km_core)
    target_include_directories(km_store_probe.elf PRIVATE core/include port/n32h48x)
    target_compile_options(km_store_probe.elf PRIVATE -Wall -Wextra -Werror -Os)
    target_link_options(km_store_probe.elf PRIVATE
        -T${CMAKE_CURRENT_SOURCE_DIR}/port/n32h48x/app.ld -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_store_probe.map)
    set_target_properties(km_store_probe.elf PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/port/n32h48x/app.ld)
    add_custom_command(TARGET km_store_probe.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_store_probe.elf km_store_probe.bin)
    target_compile_definitions(km_k2.elf PRIVATE KM_M4_ALL_KEYS=0 "KM_M4_NAME=\"KMFW-K2\"")
    target_compile_definitions(km_k3.elf PRIVATE KM_M4_ALL_KEYS=1 "KM_M4_NAME=\"KMFW-K3\"")
    # M5 L3-0 (T-E): K3 behaviour plus health monitoring, progress-gated watchdog, report reconciliation. Build only until approved.
    # km_l30s / km_l30d are the "scan stalls" hardware-test variants (recoverable / not recoverable), see KM_L30_STALL_TEST in m5_stab_main.c
    foreach(V l30 l30s l30d)
        add_executable(km_${V}.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/reset_cause.c port/n32h48x/m5_stab_main.c)
        target_link_libraries(km_${V}.elf PRIVATE km_sense km_core km_usb km_hid)
        target_include_directories(km_${V}.elf PRIVATE core/include)
        target_compile_options(km_${V}.elf PRIVATE -Wall -Wextra -Werror -Os)
    endforeach()
    target_compile_definitions(km_l30s.elf PRIVATE KM_L30_STALL_TEST=1 "KM_M5_NAME=\"KMFW-L30S\"")
    target_compile_definitions(km_l30d.elf PRIVATE KM_L30_STALL_TEST=2 "KM_M5_NAME=\"KMFW-L30D\"")
    # M5 L3-1 / L3-3 (m5_main.c): L3-0 plus trigger settings, layers, settings and the 0xC6 config protocol.
    # km_l31 keeps the settings in RAM only (no flash write except the boot flag); km_l33 also stores them in flash (store_flash.c).
    foreach(V l31 l33)
        set(M5_SRC port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/reset_cause.c port/n32h48x/m5_main.c)
        if(V STREQUAL "l33")
            list(APPEND M5_SRC port/n32h48x/store_flash.c)
        endif()
        add_executable(km_${V}.elf ${M5_SRC})
        target_link_libraries(km_${V}.elf PRIVATE km_sense km_core km_usb km_hid)
        target_include_directories(km_${V}.elf PRIVATE core/include port/n32h48x)
        target_compile_options(km_${V}.elf PRIVATE -Wall -Wextra -Werror -Os)
    endforeach()
    target_compile_definitions(km_l31.elf PRIVATE KM_M5_PERSIST=0 "KM_M5_NAME=\"KMFW-L31\"")
    target_compile_definitions(km_l33.elf PRIVATE KM_M5_PERSIST=1 "KM_M5_NAME=\"KMFW-L33\"")
    # M5 L5 (LED_NOTES.md): the km_l31 image (settings in RAM only) plus the lights (m5_led.c, led_hw.c). Three levels of hardware access:
    #   km_l41  full: PB8 / SPI5 / DMA1 ch5 and PF5 driven low (open drain) like the stock firmware
    #   km_l41n PB8 / SPI5 / DMA1 ch5, PF5 NOT touched
    #   km_l41d dry run: the whole LED pipeline runs, no LED register is touched at all
    foreach(V l41 l41n l41d l51)
        set(L5_SRC port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/reset_cause.c
                   port/n32h48x/m5_main.c port/n32h48x/m5_led.c port/n32h48x/led_hw.c)
        if(V STREQUAL "l51")
            list(APPEND L5_SRC port/n32h48x/store_flash.c)
        endif()
        add_executable(km_${V}.elf ${L5_SRC})
        target_link_libraries(km_${V}.elf PRIVATE km_sense km_core km_usb km_hid)
        target_include_directories(km_${V}.elf PRIVATE core/include port/n32h48x)
        target_compile_options(km_${V}.elf PRIVATE -Wall -Wextra -Werror -Os)
    endforeach()
    target_compile_definitions(km_l41.elf PRIVATE KM_M5_PERSIST=0 KM_M5_LED=1 KM_LED_HW_LEVEL=2 "KM_M5_NAME=\"KMFW-L41\"")
    target_compile_definitions(km_l41n.elf PRIVATE KM_M5_PERSIST=0 KM_M5_LED=1 KM_LED_HW_LEVEL=1 "KM_M5_NAME=\"KMFW-L41N\"")
    target_compile_definitions(km_l41d.elf PRIVATE KM_M5_PERSIST=0 KM_M5_LED=1 KM_LED_HW_LEVEL=0 "KM_M5_NAME=\"KMFW-L41D\"")
    # km_l51: the km_l33 image (settings and calibration kept in flash) plus the lights at full hardware level, the daily-use build
    target_compile_definitions(km_l51.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 "KM_M5_NAME=\"KMFW-L51\"")
    set(KM_LD ${CMAKE_CURRENT_SOURCE_DIR}/port/n32h48x/app.ld)
    target_link_options(km_smoke.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_smoke.map)
    set_target_properties(km_smoke.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    target_link_options(km_crashtest.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_crashtest.map)
    set_target_properties(km_crashtest.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    target_link_options(km_d1.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_d1.map)
    set_target_properties(km_d1.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    target_link_options(km_d2.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_d2.map)
    set_target_properties(km_d2.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    target_link_options(km_d3.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_d3.map)
    set_target_properties(km_d3.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    target_link_options(km_d4.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_d4.map)
    set_target_properties(km_d4.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    target_link_options(km_k1.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_k1.map)
    set_target_properties(km_k1.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    add_custom_command(TARGET km_k1.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_k1.elf km_k1.bin)
    foreach(S s1 s2 s3)
        target_link_options(km_${S}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_${S}.map)
        set_target_properties(km_${S}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_${S}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_${S}.elf km_${S}.bin)
    endforeach()
    foreach(V k2 k3)
        target_link_options(km_${V}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_${V}.map)
        set_target_properties(km_${V}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_${V}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_${V}.elf km_${V}.bin)
    endforeach()
    foreach(V l31 l33 l41 l41n l41d l51)
        target_link_options(km_${V}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_${V}.map)
        set_target_properties(km_${V}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_${V}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_${V}.elf km_${V}.bin)
    endforeach()
    foreach(V l30 l30s l30d)
        target_link_options(km_${V}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_${V}.map)
        set_target_properties(km_${V}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_${V}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_${V}.elf km_${V}.bin)
    endforeach()
    add_custom_command(TARGET km_d4.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_d4.elf km_d4.bin)
    add_custom_command(TARGET km_d3.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_d3.elf km_d3.bin)
    add_custom_command(TARGET km_d2.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_d2.elf km_d2.bin)
    add_custom_command(TARGET km_d1.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_d1.elf km_d1.bin)
    add_custom_command(TARGET km_crashtest.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_crashtest.elf km_crashtest.bin)
    add_custom_command(TARGET km_smoke.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_smoke.elf km_smoke.bin
        COMMAND ${CMAKE_SIZE} km_smoke.elf
        COMMENT "objcopy -> km_smoke.bin")
    # M6 P12a (core/NOISE_NOTES.md): noise-measurement DIAGNOSTIC image, never the daily image. L3-0 behaviour plus the raw sample
    # capture (km_capture); no lights, no settings store. Registered here only so that it can be built and checked locally;
    # the integrator may move these lines.
    add_executable(km_noise.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/reset_cause.c port/n32h48x/noise_main.c)
    target_link_libraries(km_noise.elf PRIVATE km_sense km_core km_usb km_hid)
    target_include_directories(km_noise.elf PRIVATE core/include port/n32h48x)
    target_compile_options(km_noise.elf PRIVATE -Wall -Wextra -Werror -Os)
    target_compile_definitions(km_noise.elf PRIVATE "KM_M5_NAME=\"KMFW-NOISE\"")
    target_link_options(km_noise.elf PRIVATE
        -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_noise.map)
    set_target_properties(km_noise.elf PROPERTIES LINK_DEPENDS ${KM_LD})
    add_custom_command(TARGET km_noise.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary km_noise.elf km_noise.bin)
    # M6 P10 (m6_main.c, M6_TEST_GUIDE.md): the integrated M6 images. Both have the lights, the flash persistence (settings, calibration, light
    # configuration, light scene), SOCD / Toggle / Mod-Morph / DKS / gamepad logic, the stream, the animation player and protocol version 2.
    #   km_m6a  KM_M6_PAD=0  "KMFW-M6A"  the USB descriptors are exactly those of km_l51 (no gamepad interface, whatever the settings say)
    #   km_m6   KM_M6_PAD=1  "KMFW-M6"   as km_m6a, and settings device.pad_mode = 1 presents the gamepad interface (6 s fall-back)
    # A clip for the animation player is linked in when -DKM_BADAPPLE_C=<file.c from tools/badapple_convert.py --c-array> is given (not in the repo).
    set(KM_BADAPPLE_C "" CACHE FILEPATH "optional C array of an animation clip (symbols km_anim_clip, km_anim_clip_len)")
    foreach(V m6a m6 m6p m6h m6q m6l m6hq m6hl m6f m6hf m6d)
        set(M6_SRC port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/reset_cause.c
                   port/n32h48x/m6_main.c port/n32h48x/m5_led.c port/n32h48x/led_hw.c port/n32h48x/store_flash.c)
        if(NOT KM_BADAPPLE_C STREQUAL "")
            list(APPEND M6_SRC ${KM_BADAPPLE_C})
        endif()
        add_executable(km_${V}.elf ${M6_SRC})
        if(V STREQUAL "m6h")
            target_link_libraries(km_${V}.elf PRIVATE km_sense km_core km_usb_8k km_hid)    # PUBLIC define: m6_main.c / usb_hs.c see the same value
        elseif(V STREQUAL "m6hq" OR V STREQUAL "m6hl" OR V STREQUAL "m6hf")
            target_link_libraries(km_${V}.elf PRIVATE km_sense km_core_f km_usb_8k km_hid)  # both twins: the 8 kHz descriptors and the filter modes
        elseif(V STREQUAL "m6q" OR V STREQUAL "m6l" OR V STREQUAL "m6f")
            target_link_libraries(km_${V}.elf PRIVATE km_sense km_core_f km_usb km_hid)     # PUBLIC define KM_ENGINE_FILTER_MODES=1: m6_main.c sees it too
        else()
            target_link_libraries(km_${V}.elf PRIVATE km_sense km_core km_usb km_hid)
        endif()
        target_include_directories(km_${V}.elf PRIVATE core/include port/n32h48x)
        target_compile_options(km_${V}.elf PRIVATE -Wall -Wextra -Werror -Os)
        target_link_options(km_${V}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_${V}.map)
        set_target_properties(km_${V}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_${V}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_${V}.elf km_${V}.bin)
    endforeach()
    target_compile_definitions(km_m6a.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 "KM_M5_NAME=\"KMFW-M6A\"")
    target_compile_definitions(km_m6.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=1 "KM_M5_NAME=\"KMFW-M6\"")
    # km_m6p: km_m6a plus the press-to-report probe (scope tag 0x0600, LATENCY_NOTES.md section 1). Same USB descriptors as km_m6a / km_l51.
    target_compile_definitions(km_m6p.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 "KM_M5_NAME=\"KMFW-M6P\"")
    # km_m6h: km_m6p plus the 8 kHz keyboard endpoints (bInterval 1) and bcdDevice 0x0003: a NEW descriptor set (LATENCY_NOTES.md section 4, recovery: ESC + left CTRL while plugging in).
    target_compile_definitions(km_m6h.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FASTPATH=1 "KM_M5_NAME=\"KMFW-M6H\"")
    # km_m6q / km_m6l: km_m6p (same USB descriptors, same probe) plus the adaptive engine filters and fast confirm (FILTER_NOTES.md section 7):
    # m6q = quiet (filter mode 2), m6l = low latency (filter mode 1). Hardware A/B images for the rhythm-game test; km_m6a is untouched.
    target_compile_definitions(km_m6q.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FILTER_MODE=2 "KM_M5_NAME=\"KMFW-M6Q\"")
    target_compile_definitions(km_m6l.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FILTER_MODE=1 "KM_M5_NAME=\"KMFW-M6L\"")
    # km_m6hq / km_m6hl: the same two filter modes on the 8 kHz image km_m6h (new descriptor set, bcdDevice 0x0003; recovery: ESC + left CTRL while plugging in).
    target_compile_definitions(km_m6hq.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FASTPATH=1 KM_M6_FILTER_MODE=2 "KM_M5_NAME=\"KMFW-M6HQ\"")
    target_compile_definitions(km_m6hl.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FASTPATH=1 KM_M6_FILTER_MODE=1 "KM_M5_NAME=\"KMFW-M6HL\"")
    # km_m6f / km_m6hf: filter mode 3 ("fast", FILTER_NOTES.md section 12) on the 1 kHz / 8 kHz descriptor sets: the chain designed for the MEASURED noise.
    target_compile_definitions(km_m6f.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FILTER_MODE=3 "KM_M5_NAME=\"KMFW-M6F\"")
    target_compile_definitions(km_m6hf.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_LAT_PROBE=1 KM_M6_FASTPATH=1 KM_M6_FILTER_MODE=3 "KM_M5_NAME=\"KMFW-M6HF\"")
    # km_m6d: km_m6a plus the calibration drift compensation, ON from boot (KM_M6_DRIFT=2, DRIFT_NOTES.md). Same USB descriptors as km_m6a.
    target_compile_definitions(km_m6d.elf PRIVATE KM_M5_PERSIST=1 KM_M5_LED=1 KM_LED_HW_LEVEL=2 KM_M6_PAD=0 KM_M6_DRIFT=2 "KM_M5_NAME=\"KMFW-M6D\"")
    # M6 P5b: USB interface work of the gamepad on its own (usb_diag_main.c): u5a = capability readout (plain descriptor),
    # u5b = pad descriptor + sweep, u5c = fall-back drill. DIAGNOSTIC images, never the daily image (USB_PAD_NOTES.md, M6_TEST_GUIDE.md).
    foreach(M a=0 b=1 c=2)
        string(REPLACE "=" ";" MP ${M})
        list(GET MP 0 UL)
        list(GET MP 1 UM)
        add_executable(km_u5${UL}.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/usb_diag_main.c)
        target_link_libraries(km_u5${UL}.elf PRIVATE km_usb km_hid)
        target_include_directories(km_u5${UL}.elf PRIVATE core/include)
        target_compile_definitions(km_u5${UL}.elf PRIVATE KM_USBDIAG_MODE=${UM})
        target_compile_options(km_u5${UL}.elf PRIVATE -Wall -Wextra -Werror -Os)
        target_link_options(km_u5${UL}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_u5${UL}.map)
        set_target_properties(km_u5${UL}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_u5${UL}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_u5${UL}.elf km_u5${UL}.bin)
    endforeach()
else()
    # ---- host unit tests ----
    file(GLOB KM_TEST_SRC CONFIGURE_DEPENDS tests/*.c)
    add_executable(km_tests ${KM_TEST_SRC})
    target_link_libraries(km_tests PRIVATE km_core km_hid km_usb)
    target_include_directories(km_tests PRIVATE tests)
    target_compile_options(km_tests PRIVATE -Wall -Wextra -Werror)
    enable_testing()
    add_test(NAME km_tests COMMAND km_tests)
endif()
