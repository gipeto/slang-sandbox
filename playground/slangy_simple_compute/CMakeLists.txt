
# list the shaders to compile into modules
set(SHADER_SOURCES shaders/shader_lib.slang
                   shaders/shader_main.slang)

# Create an interface target allowing to access the headers containing
# the content of the compiled shaders.
# Each shader source will be compiled into a slang module and the resulting binary
# blob written in a std::array<uint8_t> contained in a header file.
add_slang_target(computeMain FILES ${SHADER_SOURCES} NAMESPACE shaders)

add_executable(runComputeShader runComputeShader.cpp)

# link the slangy library, and the interface target containing the compiled shaders
target_link_libraries(runComputeShader slangy::slangy shaders::computeMain)

install(TARGETS runComputeShader
        INCLUDES
         DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
         DESTINATION ${CMAKE_INSTALL_BINDIRs})
