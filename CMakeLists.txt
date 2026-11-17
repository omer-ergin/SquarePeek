cmake_minimum_required(VERSION 3.20)
project(StlPreviewExplorer LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
    # Security hardening (all binaries): Control Flow Guard, CET shadow stack compatibility, ASLR.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/guard:cf>)
    add_link_options(/guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA)
endif()

# Shaders are compiled with fxc at build time (no D3DCompile / d3dcompiler_47.dll at run time).
# fxc is in the Windows SDK; the VsDevCmd called by build.cmd adds it to PATH.
find_program(FXC_EXECUTABLE fxc
    HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64"
)
if(NOT FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe not found (Windows SDK). Build with build.cmd (in a VS developer environment).")
endif()
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")
set(SHADER_HEADERS)
foreach(shader IN ITEMS
        "model.hlsl|VSMain|vs_5_0" "model.hlsl|PSMain|ps_5_0"
        "line.hlsl|VSLine|vs_5_0" "line.hlsl|PSLine|ps_5_0"
        "edge.hlsl|VSEdge|vs_5_0" "edge.hlsl|PSEdge|ps_5_0"
        "background.hlsl|VSFullscreen|vs_5_0" "background.hlsl|PSBackground|ps_5_0"
        "blit.hlsl|PSBlit|ps_5_0")
    string(REPLACE "|" ";" parts "${shader}")
    list(GET parts 0 shader_file)
    list(GET parts 1 shader_entry)
    list(GET parts 2 shader_profile)
    set(shader_header "${SHADER_OUT_DIR}/shader_${shader_entry}.h")
    add_custom_command(
        OUTPUT "${shader_header}"
        COMMAND "${FXC_EXECUTABLE}" /nologo /Ges /T ${shader_profile} /E ${shader_entry}
                /Vn g_${shader_entry} /Fh "${shader_header}" "${CMAKE_SOURCE_DIR}/src/shaders/${shader_file}"
        DEPENDS "${CMAKE_SOURCE_DIR}/src/shaders/${shader_file}"
        COMMENT "fxc ${shader_file} ${shader_entry}"
        VERBATIM
    )
    list(APPEND SHADER_HEADERS "${shader_header}")
endforeach()
add_custom_target(stlpreview_shaders DEPENDS ${SHADER_HEADERS})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

add_library(stlpreview SHARED
    src/dllmain.cpp
    src/stl_mesh.cpp
    src/model_loader.cpp
    src/model_3mf.cpp
    src/renderer.cpp
    src/overlay.cpp
    src/settings.cpp
    src/open_with.cpp
    src/thumbnail_provider.cpp
    src/preview_handler.cpp
    src/registration.cpp
    src/com_util.cpp
    src/exports.def
    src/app.rc
)
target_include_directories(stlpreview PRIVATE src "${SHADER_OUT_DIR}")
add_dependencies(stlpreview stlpreview_shaders)
target_link_libraries(stlpreview PRIVATE
    d3d11 dxgi d2d1 dwrite xmllite ole32 oleaut32 uuid shlwapi advapi32 user32 gdi32 dwmapi
    shell32 windowscodecs
)
set_target_properties(stlpreview PROPERTIES
    OUTPUT_NAME "STLPreview"
    PREFIX ""
)

set_target_properties(stlpreview PROPERTIES ENABLE_EXPORTS OFF)

add_executable(stlprev-setup
    src/setup/main.cpp
    src/registration.cpp
    src/com_util.cpp
    src/app.rc
)
target_include_directories(stlprev-setup PRIVATE src)
target_link_libraries(stlprev-setup PRIVATE ole32 advapi32 d3d11 shlwapi)

add_executable(stlprev-test
    tools/test_main.cpp
    tools/reg_test.cpp
    src/registration.cpp
    src/stl_mesh.cpp
    src/model_loader.cpp
    src/model_3mf.cpp
    src/renderer.cpp
    src/overlay.cpp
    src/settings.cpp
    src/open_with.cpp
    src/com_util.cpp
)
target_include_directories(stlprev-test PRIVATE src "${SHADER_OUT_DIR}")
add_dependencies(stlprev-test stlpreview_shaders)
target_link_libraries(stlprev-test PRIVATE
    d3d11 dxgi d2d1 dwrite xmllite ole32 oleaut32 uuid shlwapi user32 gdi32 advapi32
    shell32 windowscodecs
)

# Icon and version info (src/app.rc, assets/app.ico): embedded in the three binaries.
set_source_files_properties(src/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/app.ico")
target_compile_definitions(stlpreview PRIVATE $<$<COMPILE_LANGUAGE:RC>:STLPREV_RC_DLL>)
target_compile_definitions(stlprev-setup PRIVATE $<$<COMPILE_LANGUAGE:RC>:STLPREV_RC_SETUP>)

find_program(MAKENSIS_EXECUTABLE
    NAMES makensis
    HINTS "$ENV{SCOOP}/apps/nsis/current" "$ENV{USERPROFILE}/scoop/apps/nsis/current"
)
if(MAKENSIS_EXECUTABLE)
    add_custom_target(installer
        COMMAND "${MAKENSIS_EXECUTABLE}" /V2
                "/DSOURCE_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
                "/DOUT_DIR=${CMAKE_SOURCE_DIR}/dist"
                "${CMAKE_SOURCE_DIR}/installer/STLPreview.nsi"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building the STLPreview-Setup.exe installer"
        USES_TERMINAL
    )
    add_dependencies(installer stlpreview stlprev-setup)
endif()
