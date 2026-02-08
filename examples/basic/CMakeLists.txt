# Test executable
add_executable(test_inline
    main.c
)

target_link_libraries(test_inline
    PRIVATE inline
)
