set(d3d12_xs_headers "${BX_ROOT_DIR}/shims/d3d12_xs")
if(NOT EXISTS "${d3d12_xs_headers}/vmem.h" OR NOT EXISTS "${d3d12_xs_headers}/presentx.h")
    message(FATAL_ERROR "test_m2_first_frame requires the placed-resource and PresentX contracts (#486 and #471)")
endif()

add_executable(test_m2_first_frame main.cpp)
target_include_directories(test_m2_first_frame PRIVATE
    "${BX_ABI_INCLUDE_DIR}"
    "${d3d12_xs_headers}"
    "${BX_ROOT_DIR}/shader-transpiler/crates/d3dmetal-harness/probe/shaders"
)
target_link_libraries(test_m2_first_frame PRIVATE d3d12_xs xmem bx_runtime_veh user32)
target_compile_options(test_m2_first_frame PRIVATE -fexceptions)
