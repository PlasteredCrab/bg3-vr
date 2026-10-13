add_executable(bg3vr_xr_test xr_test.cpp)
target_link_libraries(bg3vr_xr_test PRIVATE bg3vr_xr bg3vr_warnings)
add_custom_command(TARGET bg3vr_xr_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:bg3vr_xr_test>)
