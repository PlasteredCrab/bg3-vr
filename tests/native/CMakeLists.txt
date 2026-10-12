add_executable(bg3vr_tests test_main.cpp)
target_link_libraries(bg3vr_tests PRIVATE bg3vr_base)
add_test(NAME bg3vr_tests COMMAND bg3vr_tests)
