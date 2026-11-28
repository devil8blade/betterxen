# M2-C2 (#465): rendered frames through d3d12_xs.dll's presentation bridge, verified by readback.
add_executable(test_d3d12_presentx main.cpp)
target_include_directories(test_d3d12_presentx PRIVATE "${BX_ABI_INCLUDE_DIR}" "${BX_ROOT_DIR}/shims/d3d12_xs")
target_link_libraries(test_d3d12_presentx PRIVATE d3d12_xs bx_runtime_veh user32 kernel32)
