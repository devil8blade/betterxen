# Synthetic conformance test for Xbox Series DirectStorage COM interfaces (IDStorage*X).
add_executable(test_xcrdapi_xbox main.cpp)
target_compile_definitions(test_xcrdapi_xbox PRIVATE WIDL_EXPLICIT_AGGREGATE_RETURNS)
target_include_directories(test_xcrdapi_xbox PRIVATE "${BX_ABI_INCLUDE_DIR}" "${BX_ROOT_DIR}/shims/xcrdapi/dstorage")
target_link_libraries(test_xcrdapi_xbox PRIVATE xcrdapi bx_runtime_veh kernel32)
