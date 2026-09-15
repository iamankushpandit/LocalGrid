# lg_core: portable LocalGrid protocol and messaging logic.
# Pure C11. No ESP-IDF headers, so the same sources build as an ESP-IDF
# component on the boards and as a static library for host tests.

set(LG_CORE_SRCS
    src/lg_envelope.c
    src/lg_dedup.c
    src/lg_body.c
    src/lg_roster.c
    src/lg_node.c
    src/lg_client.c
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${LG_CORE_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(lg_core STATIC ${LG_CORE_SRCS})
    target_include_directories(lg_core PUBLIC include)
    set_target_properties(lg_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(lg_core PRIVATE -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
endif()
