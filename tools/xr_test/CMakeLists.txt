add_executable(bg3vr_xr_test xr_test.cpp)
target_link_libraries(bg3vr_xr_test PRIVATE openxr_loader d3d11 dxgi bg3vr_warnings)
target_compile_definitions(bg3vr_xr_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_custom_command(TARGET bg3vr_xr_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:bg3vr_xr_test>)
