# Build inline library

if (MSVC)
    add_compile_options(/wd5045) # QSpectre warnings
    add_compile_options(/wd4668) # Macro warnings from windows libraries
    add_compile_options(/wd4820) # Padding warnings
    add_compile_options(/wd4710) # inlining of functions
    add_compile_options(/wd4711) # inlining of functions
endif()

# Library source files
add_library(inline STATIC
    inline.c    inline.h
)

target_include_directories(inline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Enable warnings for the library
target_compile_options(inline PRIVATE
    -Wall
)

# Install products
install(TARGETS inline
    EXPORT inlineTargets
    ARCHIVE DESTINATION lib        
    LIBRARY DESTINATION lib        
    RUNTIME DESTINATION bin        
)

# Headers
install(FILES inline.h
    DESTINATION include/inline
)
