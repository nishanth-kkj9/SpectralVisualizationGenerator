cmake_minimum_required(VERSION 3.20)
project(SpectralVisualizationGenerator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

# S2 — one authoritative compiled core. All production .cpps live here;
# apps/tests/benchmarks link SpectralCore instead of recompiling them.
add_library(SpectralCore STATIC
    src/core/batch/batch.cpp
    src/core/encoding/video_encoder.cpp
    src/core/encoding/video_renderer.cpp
    src/core/media/media_decoder.cpp
    src/core/pipeline/pipeline.cpp
    src/core/process/safe_process.cpp
    src/core/project/project_config.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/png_encoder.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
)
target_include_directories(SpectralCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/audio>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/batch>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/pipeline>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/process>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/project>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
  target_compile_options(SpectralCore INTERFACE /Wall)
else()
  target_compile_options(SpectralCore INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_executable(spectragen
    apps/spectragen/main.cpp
)
target_include_directories(spectragen PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/pipeline>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/batch>
)
target_compile_definitions(spectragen PRIVATE NOMINMAX)
target_link_libraries(spectragen PRIVATE SpectralCore)

set_target_properties(spectragen PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)

set_target_properties(spectragen PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)

# --- --- Phase 18: SpectraScope Qt GUI (thin client over pipeline) ---
find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(spectra_gui
        apps/spectrascope/main.cpp
        apps/spectrascope/main_window.cpp
        apps/spectrascope/worker.cpp
    )
    target_include_directories(spectra_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/media
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/pipeline
        ${CMAKE_CURRENT_SOURCE_DIR}/apps/spectrascope
    )
    target_compile_definitions(spectra_gui PRIVATE NOMINMAX)
    target_link_libraries(spectra_gui PRIVATE SpectralCore Qt6::Widgets)
    set_target_properties(spectra_gui PROPERTIES
        CXX_VISIBILITY_PRESET "hidden"
        CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
        WIN32_EXECUTABLE ON
    )
    install(TARGETS spectra_gui RUNTIME DESTINATION bin)
endif()

enable_testing()

add_test(NAME spectragen_basic
    COMMAND spectragen --help
)

# Phase 9 — CLI integration tests
add_executable(test_spectragen_cli
    tests/cli/test_spectragen_cli.cpp
)
target_link_libraries(test_spectragen_cli PRIVATE SpectralCore)

set_target_properties(test_spectragen_cli PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)

add_test(NAME spectragen_cli
    COMMAND test_spectragen_cli $<TARGET_FILE:spectragen>
)

add_test(NAME spectralcore_existence
    COMMAND ${CMAKE_COMMAND} -E echo "SpectralCore library exists"
)

add_executable(spectral_benchmarks
    src/benchmarks/main.cpp
    src/benchmarks/bench_fft.cpp
    src/benchmarks/bench_stft.cpp
    src/benchmarks/bench_dataset_serialization.cpp
    src/benchmarks/bench_image_rendering.cpp
    src/benchmarks/bench_png_encoding.cpp
    src/benchmarks/bench_audio_conversion.cpp
    src/benchmarks/bench_media_decode.cpp
    src/benchmarks/bench_video_rendering.cpp
    src/benchmarks/bench_backend.cpp
    src/benchmarks/bench_gpu_render.cpp
)
target_include_directories(spectral_benchmarks PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/core/dsp
    ${CMAKE_SOURCE_DIR}/src/core/media
    ${CMAKE_SOURCE_DIR}/src/core/rendering
    ${CMAKE_SOURCE_DIR}/src/core/spectral
    ${CMAKE_SOURCE_DIR}/src/core/encoding
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(spectral_benchmarks PRIVATE NOMINMAX)
target_link_libraries(spectral_benchmarks PRIVATE SpectralCore)

set_target_properties(spectral_benchmarks PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)

set_target_properties(spectral_benchmarks PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)

if(DEFINED ENV{FFMPEG_BINARY})
    set(_ffmpeg_binary ${ENV{FFMPEG_BINARY}})
else()
    set(_ffmpeg_binary "ffmpeg")
endif()

if(DEFINED ENV{FFPROBE_BINARY})
    set(_ffprobe_binary ${ENV{FFPROBE_BINARY}})
else()
    set(_ffprobe_binary "ffprobe")
endif()

add_executable(media_probe
    apps/media-probe/main.cpp
)
target_link_libraries(media_probe PRIVATE SpectralCore)

add_executable(test_audio_buffer
    tests/audio/test_audio_buffer.cpp
)
target_link_libraries(test_audio_buffer PRIVATE SpectralCore)
set_target_properties(test_audio_buffer PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME audio_buffer COMMAND test_audio_buffer)

add_executable(test_media
    tests/media/test_media_decoder.cpp
)
target_link_libraries(test_media PRIVATE SpectralCore)
set_target_properties(test_media PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME media_decoder COMMAND test_media)

add_executable(test_safe_process
    tests/process/test_safe_process.cpp
)
target_link_libraries(test_safe_process PRIVATE SpectralCore)
set_target_properties(test_safe_process PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME safe_process COMMAND test_safe_process)

set_target_properties(media_probe PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)

if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed")
endif()

install(TARGETS spectragen RUNTIME DESTINATION bin)

add_executable(test_fft
    tests/dsp/test_fft.cpp
)

add_executable(test_dsp_accuracy
    tests/dsp/test_dsp_accuracy.cpp
)
target_link_libraries(test_dsp_accuracy PRIVATE SpectralCore)
set_target_properties(test_dsp_accuracy PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME dsp_accuracy COMMAND test_dsp_accuracy)

target_include_directories(test_fft PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)

target_link_libraries(test_fft PRIVATE SpectralCore)

install(TARGETS test_fft RUNTIME DESTINATION bin)

# --- --- SpectralDataset (Phase 5) ---
add_executable(test_spectral_dataset
    tests/spectral/test_spectral_dataset.cpp
)
target_include_directories(test_spectral_dataset PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_link_libraries(test_spectral_dataset PRIVATE SpectralCore)
set_target_properties(test_spectral_dataset PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME spectral_dataset
    COMMAND test_spectral_dataset
)
install(TARGETS test_spectral_dataset RUNTIME DESTINATION bin)

# --- --- Phase 6: SpectrogramRenderer + PNG encoder ---
add_executable(test_spectrogram_renderer
    tests/rendering/test_spectrogram_renderer.cpp
)
target_include_directories(test_spectrogram_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_spectrogram_renderer PRIVATE NOMINMAX)
target_link_libraries(test_spectrogram_renderer PRIVATE SpectralCore)
set_target_properties(test_spectrogram_renderer PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME spectrogram_renderer
    COMMAND test_spectrogram_renderer
)
install(TARGETS test_spectrogram_renderer RUNTIME DESTINATION bin)

# --- --- Phase 7: SpectrumRenderer ---
add_executable(test_spectrum_renderer
    tests/rendering/test_spectrum_renderer.cpp
)
target_include_directories(test_spectrum_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_spectrum_renderer PRIVATE NOMINMAX)
target_link_libraries(test_spectrum_renderer PRIVATE SpectralCore)
set_target_properties(test_spectrum_renderer PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME spectrum_renderer
    COMMAND test_spectrum_renderer
)
install(TARGETS test_spectrum_renderer RUNTIME DESTINATION bin)

# --- --- Phase 8: ProjectConfig (reproducible processing) ---
add_executable(test_project_config
    tests/cli/test_project_config.cpp
)
target_include_directories(test_project_config PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/project>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_project_config PRIVATE NOMINMAX)
target_link_libraries(test_project_config PRIVATE SpectralCore)
set_target_properties(test_project_config PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME project_config
    COMMAND test_project_config
)
install(TARGETS test_project_config RUNTIME DESTINATION bin)

# --- --- Phase 10: Video encoding + rendering (in SpectralCore) ---
add_executable(test_video_encoder
    tests/video/test_video_encoder.cpp
)
target_include_directories(test_video_encoder PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(test_video_encoder PRIVATE SpectralCore)
set_target_properties(test_video_encoder PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME video_encoder COMMAND test_video_encoder)

add_executable(test_video_renderer
    tests/video/test_video_renderer.cpp
)
target_include_directories(test_video_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_video_renderer PRIVATE NOMINMAX)
target_link_libraries(test_video_renderer PRIVATE SpectralCore)
set_target_properties(test_video_renderer PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME video_renderer COMMAND test_video_renderer)

add_executable(test_video_cli
    tests/video/test_video_cli.cpp
)
target_include_directories(test_video_cli PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(test_video_cli PRIVATE SpectralCore)
set_target_properties(test_video_cli PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME video_cli COMMAND test_video_cli $<TARGET_FILE:spectragen>)

# --- --- Phase 11: Frequency scales ---
add_executable(test_frequency_scale
    tests/dsp/test_frequency_scale.cpp
)
target_include_directories(test_frequency_scale PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
set_target_properties(test_frequency_scale PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME frequency_scale COMMAND test_frequency_scale)

add_executable(test_freqscale_render
    tests/rendering/test_freqscale_render.cpp
)
target_include_directories(test_freqscale_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_compile_definitions(test_freqscale_render PRIVATE NOMINMAX)
target_link_libraries(test_freqscale_render PRIVATE SpectralCore)
set_target_properties(test_freqscale_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME freqscale_render COMMAND test_freqscale_render)

# --- --- Phase 12: Time-frequency reassignment ---
add_executable(test_reassignment
    tests/dsp/test_reassignment.cpp
)
target_include_directories(test_reassignment PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
set_target_properties(test_reassignment PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME reassignment COMMAND test_reassignment)

add_executable(test_reassignment_render
    tests/rendering/test_reassignment_render.cpp
)
target_include_directories(test_reassignment_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_compile_definitions(test_reassignment_render PRIVATE NOMINMAX)
target_link_libraries(test_reassignment_render PRIVATE SpectralCore)
set_target_properties(test_reassignment_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME reassignment_render COMMAND test_reassignment_render)

# --- --- Phase 13: Multi-band STFT ---
add_executable(test_multiband
    tests/spectral/test_multiband.cpp
)
target_include_directories(test_multiband PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_link_libraries(test_multiband PRIVATE SpectralCore)
set_target_properties(test_multiband PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME multiband COMMAND test_multiband)

add_executable(test_multiband_compare
    tests/spectral/test_multiband_compare.cpp
)
target_include_directories(test_multiband_compare PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_link_libraries(test_multiband_compare PRIVATE SpectralCore)
set_target_properties(test_multiband_compare PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME multiband_compare COMMAND test_multiband_compare)

# --- --- Sanitizer-enabled accuracy test builds (non-MSVC only) ---
if(NOT MSVC)
    # AddressSanitizer + UndefinedBehaviorSanitizer
    add_executable(test_fft_asan
        tests/dsp/test_fft.cpp
    )
    target_include_directories(test_fft_asan PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    )
    target_link_libraries(test_fft_asan PRIVATE SpectralCore)
    target_compile_options(test_fft_asan PRIVATE
        -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
    )
    target_link_options(test_fft_asan PRIVATE
        -fsanitize=address -fsanitize=undefined
    )
    set_target_properties(test_fft_asan PROPERTIES
        CXX_VISIBILITY_PRESET "hidden"
        CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
    add_test(NAME test_fft_asan
        COMMAND test_fft_asan
    )

    # MemorySanitizer (requires Clang)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_executable(test_fft_msan
            tests/dsp/test_fft.cpp
        )
        target_include_directories(test_fft_msan PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
        )
        target_link_libraries(test_fft_msan PRIVATE SpectralCore)
        target_compile_options(test_fft_msan PRIVATE
            -fsanitize=memory -fno-omit-frame-pointer
        )
        target_link_options(test_fft_msan PRIVATE
            -fsanitize=memory
        )
        set_target_properties(test_fft_msan PROPERTIES
            CXX_VISIBILITY_PRESET "hidden"
            CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
        )
        add_test(NAME test_fft_msan
            COMMAND test_fft_msan
        )
    endif()
endif()

# --- --- Phase 16: Spectral backend interface ---
add_executable(test_phase16
    tests/spectral/test_phase16.cpp
)
target_include_directories(test_phase16 PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_link_libraries(test_phase16 PRIVATE SpectralCore)
set_target_properties(test_phase16 PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME phase16 COMMAND test_phase16)

# --- --- Phase 17: GPU spectrogram rendering (D3D11 compute) ---
add_executable(test_gpu_render
    tests/rendering/test_gpu_render.cpp
)
target_include_directories(test_gpu_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
)
target_compile_definitions(test_gpu_render PRIVATE NOMINMAX)
target_link_libraries(test_gpu_render PRIVATE SpectralCore)
set_target_properties(test_gpu_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME gpu_render COMMAND test_gpu_render)

# --- --- Phase 19: batch processing + integration tests ---
add_executable(test_batch
    tests/cli/test_batch.cpp
)
target_include_directories(test_batch PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/pipeline>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/batch>
)
target_compile_definitions(test_batch PRIVATE NOMINMAX)
target_link_libraries(test_batch PRIVATE SpectralCore)
set_target_properties(test_batch PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME batch COMMAND test_batch)

# --- --- Phase 20: production hardening tests ---
add_executable(test_hardening
    tests/cli/test_hardening.cpp
)
target_include_directories(test_hardening PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/pipeline>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/batch>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/project>
)
target_compile_definitions(test_hardening PRIVATE NOMINMAX)
target_link_libraries(test_hardening PRIVATE SpectralCore)
set_target_properties(test_hardening PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME hardening COMMAND test_hardening)

# --- --- Phase 20: MSVC AddressSanitizer smoke (fail-clearly under ASan) ---
if(MSVC)
    add_executable(test_fft_asan_msvc
        tests/dsp/test_fft.cpp
    )
    target_include_directories(test_fft_asan_msvc PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    )
    target_link_libraries(test_fft_asan_msvc PRIVATE SpectralCore)
    target_compile_options(test_fft_asan_msvc PRIVATE /fsanitize=address /Zi)
    target_link_options(test_fft_asan_msvc PRIVATE /fsanitize=address)
    set_target_properties(test_fft_asan_msvc PROPERTIES
        CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
    add_test(NAME fft_asan_msvc COMMAND test_fft_asan_msvc)
endif()

# --- --- Phase 18: CLI/GUI equivalence (shared pipeline determinism) ---
add_executable(test_equivalence
    tests/cli/test_equivalence.cpp
)
target_include_directories(test_equivalence PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/pipeline>
)
target_compile_definitions(test_equivalence PRIVATE NOMINMAX)
target_link_libraries(test_equivalence PRIVATE SpectralCore)
set_target_properties(test_equivalence PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)

# S4-D3: previously built but never executed. Takes an input wav; with a
# missing name it generates a fallback sine so CTest always has input.
add_test(NAME equivalence COMMAND test_equivalence eq_input.wav)

add_executable(test_config_validation
    tests/cli/test_config_validation.cpp
)
target_link_libraries(test_config_validation PRIVATE SpectralCore)
set_target_properties(test_config_validation PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME config_validation COMMAND test_config_validation)

add_executable(test_reproducibility
    tests/cli/test_reproducibility.cpp
)
target_link_libraries(test_reproducibility PRIVATE SpectralCore)
set_target_properties(test_reproducibility PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME reproducibility COMMAND test_reproducibility)

add_executable(test_representation
    tests/spectral/test_representation.cpp
)
target_link_libraries(test_representation PRIVATE SpectralCore)
set_target_properties(test_representation PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME representation COMMAND test_representation)

if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed")
endif()