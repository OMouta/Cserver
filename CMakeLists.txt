cmake_minimum_required(VERSION 3.10)
project(Cserver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra)
endif()

set(SRCS
    src/server.c
    src/http.c
    src/log.c
    src/threadpool.c
    src/state.c
    src/utils.c
    src/cgi.c
)

add_executable(Cserver ${SRCS})

if (WIN32)
    target_link_libraries(Cserver PRIVATE ws2_32 mswsock)
endif()

install(TARGETS Cserver DESTINATION bin)
