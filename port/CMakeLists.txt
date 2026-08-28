cmake_minimum_required(VERSION 3.20)
project(TomJerryWOW LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The original game is a 32-bit x86 binary. Structures decompiled from it assume
# 4-byte pointers, so the port is built as x86 during the incremental porting stage.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "Configure with the x86 (Win32) toolchain: cmake -A Win32")
endif()

# Gate S5d: a Windows build that swaps the D3D11 gfx backend for the SAME null-gfx
# recorder the ARM headless build uses, so TJ_DRAWLOG streams compare apples-to-apples
# (Windows-null vs ARM-null). Default OFF — the shipped Windows build is untouched.
# Configure a SEPARATE build dir with -DTJ_NULL_GFX=ON; never enable it in port/build.
option(TJ_NULL_GFX "Swap the D3D11 gfx backend for the null-gfx draw recorder (Gate S5d)" OFF)
if(TJ_NULL_GFX)
    set(TJ_GFX_SRC src/runtime/gfx/null_gfx.cpp)
    message(STATUS "TJ_NULL_GFX=ON: gfx backend = null-gfx recorder (Gate S5d reference)")
else()
    set(TJ_GFX_SRC src/runtime/gfx/d3d8.cpp)
endif()

add_executable(TomJerryWOW WIN32
    src/shell/main.cpp
    src/shell/config.cpp
    src/shell/display.cpp
    src/runtime/input/xinput_pad.cpp
)

target_include_directories(TomJerryWOW PRIVATE src)
target_compile_definitions(TomJerryWOW PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(TomJerryWOW PRIVATE
    d3d11 dxgi dxguid
    xinput          # XInput 1.4 (Win8+); swap for xinput9_1_0 for broader compat
    user32 gdi32
)

set_target_properties(TomJerryWOW PROPERTIES
    VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# --- Native runtime (Xbox platform replacement) -----------------------------
add_library(tj_runtime STATIC
    src/runtime/kernel/xboxkrnl.cpp
    ${TJ_GFX_SRC}
    src/runtime/engine/sys_video.cpp
    src/runtime/assets/xmf_texture.cpp
    src/runtime/assets/xmf_mesh.cpp
    src/runtime/assets/xbd_anim.cpp
    src/runtime/assets/tec_collision.cpp
    src/runtime/input/xinput_pad.cpp
    src/runtime/snd/xadpcm.cpp
    src/runtime/snd/xaudio_backend.cpp
    src/runtime/snd/wav_dump.cpp
)
target_include_directories(tj_runtime PUBLIC src)
target_compile_definitions(tj_runtime PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# xaudio2 = XAudio2.9 (Windows 10+ inbox, no redist); ole32 for the CoInitializeEx fallback.
target_link_libraries(tj_runtime PUBLIC d3d11 dxgi dxguid d3dcompiler xinput user32 xaudio2 ole32)

add_executable(runtime_demo WIN32 src/runtime/runtime_demo.cpp)
target_link_libraries(runtime_demo PRIVATE tj_runtime)
target_compile_definitions(runtime_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(runtime_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(sys_init_demo WIN32 src/runtime/sys_init_demo.cpp)
target_link_libraries(sys_init_demo PRIVATE tj_runtime)
target_compile_definitions(sys_init_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(sys_init_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(cube_demo WIN32 src/runtime/cube_demo.cpp)
target_link_libraries(cube_demo PRIVATE tj_runtime)
target_compile_definitions(cube_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(cube_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(level_demo WIN32 src/runtime/level_demo.cpp)
target_link_libraries(level_demo PRIVATE tj_runtime)
target_compile_definitions(level_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(level_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(play_demo WIN32 src/runtime/play_demo.cpp)
target_link_libraries(play_demo PRIVATE tj_runtime)
target_compile_definitions(play_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(play_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(bg_demo WIN32 src/runtime/bg_demo.cpp)
target_link_libraries(bg_demo PRIVATE tj_runtime)
target_compile_definitions(bg_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(bg_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Ported game code + differential correctness test -----------------------
add_library(tj_game STATIC
    src/game/data_image.cpp
    src/game/ported.cpp
)
target_include_directories(tj_game PUBLIC src)
# The original code uses x87 float math; build the ported math with x87 too so
# multiply-accumulate chains keep identical intermediate precision (byte-exact diff).
target_compile_options(tj_game PRIVATE /arch:IA32 /fp:precise)

# --- Hybrid XBE bring-up (run the original x86 game code in-process) ---------
# tj_hybrid.dll: based HIGH (above the XBE image). Maps default.xbe at real VAs,
# wires the kernel thunks + XDK bridges, and drives the game's startup.
add_library(tj_hybrid SHARED
    src/android/perf_hint.cpp
    src/hybrid/xbe_image.cpp
    src/hybrid/kernel.cpp
    src/hybrid/symbolize.cpp
    src/hybrid/xdk_patch.cpp
    src/hybrid/dispatch.cpp
    src/hybrid/crt_glue.cpp
    src/hybrid/fs_fixup.cpp
    src/hybrid/d3d8_bridge.cpp
    src/hybrid/dsound_bridge.cpp
    src/hybrid/fe_menu.cpp
    src/hybrid/audio_ui.cpp
    src/hybrid/arabic.cpp
    src/hybrid/xcrypt.cpp
    src/hybrid/net_sync.cpp
    src/hybrid/net_lan.cpp
    src/hybrid/lan_match.cpp
    src/hybrid/lan_ui.cpp
    src/hybrid/meat_rush.cpp
    src/hybrid/meat_ui.cpp
    src/hybrid/meat_menu.cpp
    src/hybrid/file_io.cpp
    src/hybrid/prof.cpp
    src/hybrid/engine_mode.cpp
    src/hybrid/startup.cpp
    src/hybrid/hybrid_run.cpp
)
target_include_directories(tj_hybrid PRIVATE src)
target_compile_definitions(tj_hybrid PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# winmm: timeBeginPeriod for the TJ_PROF sampler; psapi is in kernel32 on modern SDKs.
# tj_engine: the x86 interpreter (TJ_ENGINE=1 runs the whole game through it, Stage 3).
target_link_libraries(tj_hybrid PRIVATE tj_runtime tj_game tj_engine tj_softfloat winmm)
target_link_options(tj_hybrid PRIVATE /BASE:0x30000000 /MAP)
set_target_properties(tj_hybrid PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# tj_loader.exe: based LOW at 0x10000 with a ~23 MB committed bss that reserves the
# entire XBE VA range, forcing stack/TEB/heaps above it. Loads tj_hybrid + RunGame.
add_executable(tj_loader src/hybrid/loader_main.cpp src/hybrid/loader.rc)
target_include_directories(tj_loader PRIVATE src)
target_compile_definitions(tj_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(tj_loader PRIVATE
    /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
    /STACK:0x100000,0x100000)
set_target_properties(tj_loader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Installer (tj_setup) ----------------------------------------------------
# The installer ships NO game data: the user supplies their own disc image and it extracts
# from that. /MT so the installer itself needs no VC redist to run.
add_library(tj_setup_core STATIC src/setup/xdvdfs.cpp src/setup/xmf_inject.cpp)
target_include_directories(tj_setup_core PUBLIC src)
target_compile_definitions(tj_setup_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET tj_setup_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The installer embeds the runtime as RCDATA. Those files are build OUTPUTS, so the paths are
# resolved here and the .rc is generated. NOTE: tj_setup is a RELEASE artifact -- it embeds the
# Release binaries by path, so build it with --config Release.
set(TJ_SETUP_ICON  "${CMAKE_CURRENT_SOURCE_DIR}/src/setup/tjsetup.ico")
set(TJ_README_TXT  "${CMAKE_CURRENT_SOURCE_DIR}/tools/dist_README.txt")
set(TJ_LOADER_EXE  "${CMAKE_BINARY_DIR}/bin/Release/tj_loader.exe")
set(TJ_HYBRID_DLL  "${CMAKE_BINARY_DIR}/bin/Release/tj_hybrid.dll")

# THE ANDROID HALF. The installer builds a self-contained apk on the PLAYER'S machine with
# no Android SDK, by adding their extracted game files to a template apk and signing the
# result itself (src/setup/apk_build.cpp). The template is OURS and carries no game data,
# so embedding it is the same footing as embedding tj_hybrid.dll.
#
# It is produced OUTSIDE cmake, by:  port/tools/build_apk.ps1 -Template
# which needs the Android SDK/NDK — on THIS machine, once, not on any player's. When it is
# absent the installer still builds and simply offers no Android option (TJ_HAVE_APK).
set(TJ_APK_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/build-apk/WOTW-template.apk")
# ⚠ Read at CONFIGURE time only. If the template is REBUILT after this runs, re-configure or
# the installer keeps embedding the old one (build_apk.ps1 -Template then a fresh cmake -S/-B).
# The resource dependency below makes MSBuild relink when the file's contents change, but only
# once CMake has re-generated payload.rc from it.
if(EXISTS "${TJ_APK_TEMPLATE}")
    set(TJ_APK_RC "IDR_APK_TEMPLATE       RCDATA  \"${TJ_APK_TEMPLATE}\"")
    set(TJ_HAVE_APK 1)
    message(STATUS "installer: Android template found, the Android option is available")
else()
    set(TJ_APK_RC "// no Android template at configure time (run build_apk.ps1 -Template)")
    set(TJ_HAVE_APK 0)
    message(STATUS "installer: NO Android template — the installer will be PC-only")
endif()

# THE ARABIC PACK: glyph art + the translated string table, built by
#     port/tools/arabic_font.py --pack port/build-arabic/arabic_font.bin
# which needs Python + Pillow + a system Arabic face -- on THIS machine, not on any player's.
# It is NOT in the repo: it carries 64 signature bytes lifted from the game's own font sheet,
# and the rule is that nothing derived from the disc is ever pushed. Same footing as the
# Android template: read at CONFIGURE time, and absent means the installer ships no Arabic
# and the game stays English (arabic.cpp treats a missing pack as "feature unavailable").
set(TJ_ARABIC_PACK "${CMAKE_CURRENT_SOURCE_DIR}/build-arabic/arabic_font.bin")
if(EXISTS "${TJ_ARABIC_PACK}")
    set(TJ_ARABIC_RC "IDR_ARABIC_FONT        RCDATA  \"${TJ_ARABIC_PACK}\"")
    message(STATUS "installer: Arabic pack found, Arabic will be installed")
else()
    set(TJ_ARABIC_RC "// no Arabic pack at configure time (run tools/arabic_font.py --pack)")
    message(STATUS "installer: NO Arabic pack -- the installed game will be English-only")
endif()

# Same VC++ redist the dist folder ships. Windows does not include it; app-local copies are
# the supported way to avoid making the player install a redist.
file(GLOB TJ_CRT_DIRS "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/*/x86/Microsoft.VC*.CRT")
list(SORT TJ_CRT_DIRS)
list(REVERSE TJ_CRT_DIRS)
if(TJ_CRT_DIRS)
    list(GET TJ_CRT_DIRS 0 TJ_CRT_DIR)
    set(CRT_MSVCP140        "${TJ_CRT_DIR}/msvcp140.dll")
    set(CRT_MSVCP140_1      "${TJ_CRT_DIR}/msvcp140_1.dll")
    set(CRT_MSVCP140_2      "${TJ_CRT_DIR}/msvcp140_2.dll")
    set(CRT_MSVCP140_ATOMIC "${TJ_CRT_DIR}/msvcp140_atomic_wait.dll")
    set(CRT_VCRUNTIME140    "${TJ_CRT_DIR}/vcruntime140.dll")
    set(CRT_VCRUNTIME140_THR "${TJ_CRT_DIR}/vcruntime140_threads.dll")
    set(CRT_CONCRT140       "${TJ_CRT_DIR}/concrt140.dll")
    configure_file(src/setup/payload.rc.in ${CMAKE_BINARY_DIR}/payload.rc @ONLY)
    # The .rc pulls these in by PATH, so CMake cannot see that the installer depends on their
    # CONTENT. Without this, rebuilding tj_hybrid.dll and not rebuilding tj_setup silently
    # ships the PREVIOUS DLL -- an installer that looks fine and installs a stale game.
    # ⚠ THE ANDROID TEMPLATE BELONGS IN THIS LIST TOO, and it was missing: rebuilding the
    # template and then tj_setup relinked NOTHING, so the installer kept embedding a template
    # three commits old while every timestamp looked fresh. Same failure mode the comment
    # above describes for the DLL, one file further along.
    set(TJ_PAYLOAD_DEPS "${TJ_LOADER_EXE};${TJ_HYBRID_DLL};${TJ_SETUP_ICON};${TJ_README_TXT}")
    if(TJ_HAVE_APK)
        list(APPEND TJ_PAYLOAD_DEPS "${TJ_APK_TEMPLATE}")
    endif()
    set_source_files_properties(${CMAKE_BINARY_DIR}/payload.rc PROPERTIES
        OBJECT_DEPENDS "${TJ_PAYLOAD_DEPS}")

    add_executable(tj_setup WIN32
        src/setup/setup_main.cpp
        src/setup/payload.cpp
        src/setup/apk_build.cpp
        ${CMAKE_BINARY_DIR}/payload.rc
    )
    add_dependencies(tj_setup tj_loader tj_hybrid)
    target_link_libraries(tj_setup PRIVATE tj_setup_core comctl32 shell32 ole32 shlwapi advapi32 uuid)
    target_include_directories(tj_setup PRIVATE src)
    target_compile_definitions(tj_setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                               TJ_HAVE_APK=${TJ_HAVE_APK})
    set_property(TARGET tj_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_target_properties(tj_setup PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
    # Same installer, WITHOUT the requireAdministrator manifest, so `/silent` can be driven by
    # a test script into a scratch folder rather than needing a human to click through UAC.
    # It is a test artifact only -- the shipped installer is tj_setup.exe above.
    # ⚠ apk_build.cpp and TJ_HAVE_APK were MISSING here, so the probe -- whose entire reason
    # to exist is that `/silent` can be driven by a script -- could not exercise the Android
    # path at all. `/silent ... /android` ran, reported success, and quietly produced no apk,
    # which is indistinguishable from "the flag did not arrive" unless you read this file.
    add_executable(tj_setup_probe WIN32
        src/setup/setup_main.cpp
        src/setup/payload.cpp
        src/setup/apk_build.cpp
        ${CMAKE_BINARY_DIR}/payload.rc
    )
    add_dependencies(tj_setup_probe tj_loader tj_hybrid)
    target_link_libraries(tj_setup_probe PRIVATE tj_setup_core comctl32 shell32 ole32 shlwapi advapi32 uuid)
    target_include_directories(tj_setup_probe PRIVATE src)
    target_compile_definitions(tj_setup_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                               TJ_HAVE_APK=${TJ_HAVE_APK})
    set_property(TARGET tj_setup_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_target_properties(tj_setup_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
else()
    message(WARNING "No x86 VC++ redist found -- the tj_setup installer target is unavailable")
endif()

add_executable(xdvdfs_test src/setup/xdvdfs_test.cpp)
target_link_libraries(xdvdfs_test PRIVATE tj_setup_core)
target_compile_definitions(xdvdfs_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET xdvdfs_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(xdvdfs_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(xmf_test src/setup/xmf_test.cpp)
target_link_libraries(xmf_test PRIVATE tj_setup_core)
target_compile_definitions(xmf_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET xmf_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(xmf_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(tex_test tests/tex_test.cpp src/runtime/assets/xmf_texture.cpp)
target_include_directories(tex_test PRIVATE src)
target_compile_features(tex_test PRIVATE cxx_std_17)

add_executable(diff_test tests/diff_test.cpp)
target_link_libraries(diff_test PRIVATE tj_game)
set_target_properties(diff_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Berkeley SoftFloat-3e: extFloat80 subset, 8086 specialization ----------
# Vendored from github.com/ucb-bar/berkeley-softfloat-3 (BSD-3-Clause; license
# preserved as src/engine/softfloat/COPYING.txt). EXACT deterministic 80-bit
# x87 soft-float for the interpreter FPU (ANDROID_PLAN.md S1d): the 8086
# specialization models x87 NaN propagation, extF80_roundingPrecision models
# x87 precision control. Flattened; platform.h is the only non-upstream file
# (MSVC build glue). NOT linked into tj_engine yet.
add_library(tj_softfloat STATIC
    src/engine/softfloat/extF80_add.c
    src/engine/softfloat/extF80_sub.c
    src/engine/softfloat/extF80_mul.c
    src/engine/softfloat/extF80_div.c
    src/engine/softfloat/extF80_rem.c
    src/engine/softfloat/extF80_sqrt.c
    src/engine/softfloat/extF80_roundToInt.c
    src/engine/softfloat/extF80_eq.c
    src/engine/softfloat/extF80_eq_signaling.c
    src/engine/softfloat/extF80_le.c
    src/engine/softfloat/extF80_le_quiet.c
    src/engine/softfloat/extF80_lt.c
    src/engine/softfloat/extF80_lt_quiet.c
    src/engine/softfloat/extF80_isSignalingNaN.c
    src/engine/softfloat/extF80_to_f32.c
    src/engine/softfloat/extF80_to_f64.c
    src/engine/softfloat/extF80_to_i32.c
    src/engine/softfloat/extF80_to_i32_r_minMag.c
    src/engine/softfloat/extF80_to_i64.c
    src/engine/softfloat/extF80_to_i64_r_minMag.c
    src/engine/softfloat/f32_to_extF80.c
    src/engine/softfloat/f64_to_extF80.c
    src/engine/softfloat/i32_to_extF80.c
    src/engine/softfloat/i64_to_extF80.c
    src/engine/softfloat/softfloat_state.c
    src/engine/softfloat/softfloat_raiseFlags.c
    src/engine/softfloat/s_propagateNaNExtF80UI.c
    src/engine/softfloat/s_extF80UIToCommonNaN.c
    src/engine/softfloat/s_commonNaNToExtF80UI.c
    src/engine/softfloat/s_f32UIToCommonNaN.c
    src/engine/softfloat/s_commonNaNToF32UI.c
    src/engine/softfloat/s_f64UIToCommonNaN.c
    src/engine/softfloat/s_commonNaNToF64UI.c
    src/engine/softfloat/s_addMagsExtF80.c
    src/engine/softfloat/s_subMagsExtF80.c
    src/engine/softfloat/s_normSubnormalExtF80Sig.c
    src/engine/softfloat/s_normRoundPackToExtF80.c
    src/engine/softfloat/s_roundPackToExtF80.c
    src/engine/softfloat/s_normSubnormalF32Sig.c
    src/engine/softfloat/s_normSubnormalF64Sig.c
    src/engine/softfloat/s_roundPackToF32.c
    src/engine/softfloat/s_roundPackToF64.c
    src/engine/softfloat/s_roundToI32.c
    src/engine/softfloat/s_roundToI64.c
    src/engine/softfloat/s_add128.c
    src/engine/softfloat/s_sub128.c
    src/engine/softfloat/s_le128.c
    src/engine/softfloat/s_lt128.c
    src/engine/softfloat/s_mul64To128.c
    src/engine/softfloat/s_mul64ByShifted32To128.c
    src/engine/softfloat/s_shortShiftLeft128.c
    src/engine/softfloat/s_shiftRightJam32.c
    src/engine/softfloat/s_shiftRightJam64.c
    src/engine/softfloat/s_shiftRightJam64Extra.c
    src/engine/softfloat/s_shiftRightJam128.c
    src/engine/softfloat/s_shortShiftRightJam64.c
    src/engine/softfloat/s_shortShiftRightJam64Extra.c
    src/engine/softfloat/s_countLeadingZeros8.c
    src/engine/softfloat/s_countLeadingZeros32.c
    src/engine/softfloat/s_countLeadingZeros64.c
    src/engine/softfloat/s_approxRecip32_1.c
    src/engine/softfloat/s_approxRecip_1Ks.c
    src/engine/softfloat/s_approxRecipSqrt32_1.c
    src/engine/softfloat/s_approxRecipSqrt_1Ks.c
)
target_include_directories(tj_softfloat PUBLIC src/engine/softfloat)
# SOFTFLOAT_FAST_INT64 is PUBLIC because softfloat.h's API shape depends on it.
# LITTLEENDIAN is PUBLIC because softfloat_types.h picks the FIELD ORDER of
# extFloat80M from it -- a client TU without it silently gets the reversed
# struct layout (ABI break, caught by softfloat_smoke).
# The PRIVATE options are speed-only (per SoftFloat docs) -- none change results,
# so the engine stays bit-deterministic across compilers/platforms.
target_compile_definitions(tj_softfloat
    PUBLIC  SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1
    PRIVATE INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)

# Smoke test: hard-coded x87 bit patterns (add/round-trip/precision-control/
# sqrt/default-NaN); exits nonzero on any mismatch.
add_executable(softfloat_smoke tests/softfloat_smoke.cpp)
target_link_libraries(softfloat_smoke PRIVATE tj_softfloat)
set_target_properties(softfloat_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- x86-32 execution engine (Android plan, port/ANDROID_PLAN.md Stage 1) ----
# Interpreter + NATIVE-x87 unit + the engine gate. eng_test is diff_test's oracle
# pointed at the engine: original code executed NATIVELY vs INTERPRETED must be
# byte-identical on randomized memory (Gate S1b: the 141; Gate S1c: all 1,764).
add_library(tj_engine STATIC
    src/engine/x86_interp.cpp
    src/engine/x87_native.cpp
    src/engine/x87_exact.cpp
    src/engine/x87_fast.cpp
    src/engine/eng_gate.cpp
    src/engine/fpu_host.cpp
)
target_include_directories(tj_engine PUBLIC src)
target_compile_definitions(tj_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
# x87_exact.cpp is the EXACT soft-float unit (ANDROID_PLAN S1d); the softfloat
# PUBLIC defines (SOFTFLOAT_FAST_INT64, LITTLEENDIAN) flow in through this link.
target_link_libraries(tj_engine PRIVATE tj_softfloat)

add_executable(eng_test tests/eng_test.cpp)
target_link_libraries(eng_test PRIVATE tj_game tj_engine)
set_target_properties(eng_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Stage-5 cross-host smoke (also in port/android/CMakeLists.txt): outputs must match
# the aarch64 build line for line — a miniature Gate S5c.
add_executable(eng_smoke tests/eng_smoke.cpp)
target_link_libraries(eng_smoke PRIVATE tj_engine tj_softfloat)
set_target_properties(eng_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_executable(eng_bench tests/eng_bench.cpp)
target_link_libraries(eng_bench PRIVATE tj_engine tj_softfloat)
set_target_properties(eng_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Stage-5 CALL-BOUNDARY cross-host smoke (also in port/android/CMakeLists.txt):
# dispatch typed shims + Engine::Call marshaling, outputs line-identical across hosts.
add_executable(eng_call_smoke tests/eng_call_smoke.cpp src/hybrid/dispatch.cpp)
target_link_libraries(eng_call_smoke PRIVATE tj_engine tj_softfloat)
set_target_properties(eng_call_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Gate S1d: EXACT-vs-NATIVE x87 differential corpus (see tests/x87_corpus.cpp).
add_executable(x87_corpus tests/x87_corpus.cpp)
target_link_libraries(x87_corpus PRIVATE tj_engine tj_game tj_softfloat)
set_target_properties(x87_corpus PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Gate S1c: the signature-free full-context sweep over every .text function.
add_executable(eng_sweep tests/eng_sweep.cpp)
target_link_libraries(eng_sweep PRIVATE tj_game tj_engine)
set_target_properties(eng_sweep PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_custom_command(TARGET eng_sweep POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src/game/generated/data_image.bin
        ${CMAKE_SOURCE_DIR}/src/game/generated/text_image.bin
        ${CMAKE_SOURCE_DIR}/src/game/generated/text_relocs.bin
        ${CMAKE_SOURCE_DIR}/../re/functions.csv
        $<TARGET_FILE_DIR:eng_sweep>
)
# Stage the extracted images + relocation table next to the test binary.
add_custom_command(TARGET diff_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src/game/generated/data_image.bin
        ${CMAKE_SOURCE_DIR}/src/game/generated/text_image.bin
        ${CMAKE_SOURCE_DIR}/src/game/generated/text_relocs.bin
        $<TARGET_FILE_DIR:diff_test>
)
