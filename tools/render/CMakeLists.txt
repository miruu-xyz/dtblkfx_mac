# Offline render / regression harness for the DtBlkFx DSP core.
#
# Links the engine directly, with no JUCE and no plugin host, so a full sweep of
# every effect type runs in a couple of seconds.
add_executable(dtblkfx_render dtblkfx_render.cpp)

target_link_libraries(dtblkfx_render PRIVATE dtblkfx_core)
target_compile_features(dtblkfx_render PRIVATE cxx_std_17)
target_compile_options(dtblkfx_render PRIVATE -Wall -Wextra)

# The harness must be bit-reproducible run to run, so no fast-math and no LTO
# here even if the plugin build grows them later.
target_compile_options(dtblkfx_render PRIVATE -ffp-contract=off)

set_target_properties(dtblkfx_render PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
