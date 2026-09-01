cmake_minimum_required(VERSION 3.20)
project(obs_delay_core_tests LANGUAGES CXX)

add_executable(packet_delay_tests packet_delay_tests.cpp ../src/core/packet_delay.cpp)

target_include_directories(packet_delay_tests PRIVATE ../src/core)
target_compile_features(packet_delay_tests PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(packet_delay_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(
    packet_delay_tests
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  )
endif()

find_package(Threads REQUIRED)
target_link_libraries(packet_delay_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME packet_delay_tests COMMAND packet_delay_tests)

add_executable(audio_spsc_fifo_tests audio_spsc_fifo_tests.cpp)
target_include_directories(audio_spsc_fifo_tests PRIVATE ../src)
target_compile_features(audio_spsc_fifo_tests PRIVATE cxx_std_20)
if(MSVC)
  # Capacity 3 intentionally pads between the odd-sized slot array and the
  # first 64-byte sequence wrapper so producer/consumer cache lines stay
  # isolated. C4324 reports that deliberate layout padding at warning level 4.
  target_compile_options(audio_spsc_fifo_tests PRIVATE /W4 /permissive- /wd4324)
else()
  target_compile_options(
    audio_spsc_fifo_tests
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
  )
endif()
target_link_libraries(audio_spsc_fifo_tests PRIVATE Threads::Threads)
add_test(NAME audio_spsc_fifo_tests COMMAND audio_spsc_fifo_tests)
