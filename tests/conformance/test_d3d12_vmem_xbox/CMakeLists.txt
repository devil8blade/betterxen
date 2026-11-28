# Synthetic conformance test for the Xbox 32-byte D3D12ConfigureVirtualMemoryX decoder.
add_executable(test_d3d12_vmem_xbox main.cpp)
target_include_directories(test_d3d12_vmem_xbox PRIVATE "${BX_ABI_INCLUDE_DIR}")
target_link_libraries(test_d3d12_vmem_xbox PRIVATE d3d12_xs xmem bx_runtime_veh kernel32)
