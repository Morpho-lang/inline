# Test executable
add_executable(calc
    main.c
)

target_link_libraries(calc
    PRIVATE inline
)

install(TARGETS calc
    RUNTIME DESTINATION bin
)
