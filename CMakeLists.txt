cmake_minimum_required(VERSION 3.20)
project(SpectralVisualizationGenerator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug")
endif()

add_library(SpectralCore INTERFACE)
target_include_directories(SpectralCore INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
  target_compile_options(SpectralCore INTERFACE /Wall)
else()
  target_compile_options(SpectralCore INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_executable(spectragen
    src/cli/main.cpp
    src/core/media/media_decoder.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
    src/core/encoding/video_encoder.cpp
    src/core/encoding/video_renderer.cpp
)
target_include_directories(spectragen PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/media>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_compile_definitions(spectragen PRIVATE NOMINMAX)
target_link_libraries(spectragen PRIVATE SpectralCore d3d11 d3dcompiler)

set_target_properties(spectragen PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)

set_target_properties(spectragen PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:CONSOLE"
)

enable_testing()

add_test(NAME spectragen_basic
    COMMAND spectragen --help
)

# Phase 9 — CLI integration tests
add_executable(test_spectragen_cli
    tests/phase9/test_spectragen_cli.cpp
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
    src/core/spectral/spectral_dataset.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/png_encoder.cpp
    src/core/media/media_decoder.cpp
    src/core/encoding/video_encoder.cpp
    src/core/encoding/video_renderer.cpp
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
target_link_libraries(spectral_benchmarks PRIVATE SpectralCore d3d11 d3dcompiler)

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
    src/core/media/media_decoder.cpp
    src/core/media/main.cpp
)

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
    src/core/dsp/test_fft.cpp
)

target_include_directories(test_fft PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)

target_link_libraries(test_fft PRIVATE SpectralCore)

install(TARGETS test_fft RUNTIME DESTINATION bin)

# --- --- SpectralDataset (Phase 5) ---
add_executable(test_spectral_dataset
    src/core/spectral/spectral_dataset.cpp
    src/core/spectral/test_spectral_dataset.cpp
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
    src/core/rendering/png_encoder.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/test_spectrogram_renderer.cpp
)
target_include_directories(test_spectrogram_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_spectrogram_renderer PRIVATE NOMINMAX)
target_link_libraries(test_spectrogram_renderer PRIVATE SpectralCore d3d11 d3dcompiler)
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
    src/core/rendering/png_encoder.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/test_spectrum_renderer.cpp
)
target_include_directories(test_spectrum_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_spectrum_renderer PRIVATE NOMINMAX)
target_link_libraries(test_spectrum_renderer PRIVATE SpectralCore d3d11 d3dcompiler)
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
    src/core/project/project_config.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
    src/core/project/test_project_config.cpp
)
target_include_directories(test_project_config PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/project>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_project_config PRIVATE NOMINMAX)
target_link_libraries(test_project_config PRIVATE SpectralCore d3d11 d3dcompiler)
set_target_properties(test_project_config PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME project_config
    COMMAND test_project_config
)
install(TARGETS test_project_config RUNTIME DESTINATION bin)

# --- --- Phase 10: Video encoding + rendering ---
add_library(spectral_video_encoding STATIC
    src/core/encoding/video_encoder.cpp
)
target_include_directories(spectral_video_encoding PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(spectral_video_encoding PRIVATE SpectralCore)

add_library(spectral_video_renderer STATIC
    src/core/encoding/video_renderer.cpp
)
target_include_directories(spectral_video_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_link_libraries(spectral_video_renderer PRIVATE
    SpectralCore
    spectral_video_encoding
)

add_executable(test_video_encoder
    tests/phase10/test_video_encoder.cpp
)
target_include_directories(test_video_encoder PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
)
target_link_libraries(test_video_encoder PRIVATE spectral_video_encoding)
set_target_properties(test_video_encoder PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME video_encoder COMMAND test_video_encoder)

add_executable(test_video_renderer
    tests/phase10/test_video_renderer.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/png_encoder.cpp
)
target_include_directories(test_video_renderer PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/encoding>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
)
target_compile_definitions(test_video_renderer PRIVATE NOMINMAX)
target_link_libraries(test_video_renderer PRIVATE
    spectral_video_renderer
    spectral_video_encoding
    d3d11
    d3dcompiler
)
set_target_properties(test_video_renderer PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME video_renderer COMMAND test_video_renderer)

add_executable(test_video_cli
    tests/phase10/test_video_cli.cpp
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
    tests/phase11/test_frequency_scale.cpp
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
    tests/phase11/test_freqscale_render.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
)
target_include_directories(test_freqscale_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_compile_definitions(test_freqscale_render PRIVATE NOMINMAX)
target_link_libraries(test_freqscale_render PRIVATE SpectralCore d3d11 d3dcompiler)
set_target_properties(test_freqscale_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME freqscale_render COMMAND test_freqscale_render)

# --- --- Phase 12: Time-frequency reassignment ---
add_executable(test_reassignment
    tests/phase12/test_reassignment.cpp
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
    tests/phase12/test_reassignment_render.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/spectrum_renderer.cpp
    src/core/rendering/png_encoder.cpp
)
target_include_directories(test_reassignment_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
)
target_compile_definitions(test_reassignment_render PRIVATE NOMINMAX)
target_link_libraries(test_reassignment_render PRIVATE SpectralCore d3d11 d3dcompiler)
set_target_properties(test_reassignment_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
)
add_test(NAME reassignment_render COMMAND test_reassignment_render)

# --- --- Phase 13: Multi-band STFT ---
add_executable(test_multiband
    tests/phase13/test_multiband.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
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
    tests/phase13/test_multiband_compare.cpp
    src/core/spectral/multiband_analyzer.cpp
    src/core/spectral/spectral_dataset.cpp
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
        src/core/dsp/test_fft.cpp
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
            src/core/dsp/test_fft.cpp
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
    tests/accuracy/test_phase16.cpp
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
    tests/accuracy/test_gpu_render.cpp
    src/core/spectral/spectral_dataset.cpp
    src/core/rendering/spectrogram_renderer.cpp
    src/core/rendering/gpu_spectrogram.cpp
    src/core/rendering/d3d11_context.cpp
    src/core/rendering/png_encoder.cpp
)
target_include_directories(test_gpu_render PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/spectral>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/rendering>
)
target_compile_definitions(test_gpu_render PRIVATE NOMINMAX)
target_link_libraries(test_gpu_render PRIVATE SpectralCore d3d11 d3dcompiler)
set_target_properties(test_gpu_render PROPERTIES
    CXX_VISIBILITY_PRESET "hidden"
    CXX_RUNTIME_LIBRARY "MultiThreadedDLL"
    WIN32_EXECUTABLE OFF
)
add_test(NAME gpu_render COMMAND test_gpu_render)

if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${VCPKG_ROOT}/installed")
endif()