
# list the shaders to compile into modules
set(SHADER_SOURCES shaders/vertex_shader.slang shaders/pixel_shader.slang)

# Create an interface target allowing to access the headers containing
# the content of the compiled shaders.
# Each shader source will be compiled into a slang module and the resulting binary
# blob written in a std::array<uint8_t> contained in a header file.
add_slang_target(guiSampleShaders FILES ${SHADER_SOURCES} NAMESPACE shaders)

add_executable(guiSample guiSample.cpp d3dwin.h d3dPso.h)

# link the slangy library, and the interface target containing the compiled shaders
target_link_libraries(guiSample slangy::slangy d3d12 dxgi d3dcompiler dxcompiler shaders::guiSampleShaders)
target_link_options(guiSample PRIVATE "/ENTRY:wWinMainCRTStartup")

install(TARGETS guiSample
        INCLUDES
         DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
         DESTINATION ${CMAKE_INSTALL_BINDIRs})
