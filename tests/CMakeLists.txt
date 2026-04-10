set(TEST_NAME utests)

add_executable(${TEST_NAME}
    test_basic.cpp
)

target_link_libraries(${TEST_NAME}
    GTest::gtest_main
)

gtest_discover_tests(${TEST_NAME})

