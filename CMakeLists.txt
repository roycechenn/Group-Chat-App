cmake_minimum_required(VERSION 3.22)

project(groupchat VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(Threads REQUIRED)

add_library(chat_core STATIC
  src/protocol.c
  src/connection.c
  src/config.c
)

target_include_directories(chat_core PUBLIC include)

add_executable(server src/server.c)
target_link_libraries(server PRIVATE chat_core)

add_executable(client src/client.c)
target_link_libraries(client PRIVATE chat_core Threads::Threads)

foreach(target chat_core server client)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic
  )
endforeach()

option(
  GROUP_CHAT_ENABLE_SANITIZERS
  "Enable address and undefined-behavior checks"
  OFF
)

if(GROUP_CHAT_ENABLE_SANITIZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Sanitizers require GCC or Clang")
  endif()

  foreach(target chat_core server client)
    target_compile_options(${target} PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
    )
  endforeach()

  foreach(target server client)
    target_link_options(${target} PRIVATE
      -fsanitize=address,undefined
    )
  endforeach()
endif()

include(CTest)

if(BUILD_TESTING)
  find_package(Python3 3.8 COMPONENTS Interpreter QUIET)

  if(Python3_Interpreter_FOUND)
    foreach(suite server client cli integration)
      add_test(
        NAME chat_${suite}
        COMMAND
          ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
          --server $<TARGET_FILE:server>
          --client $<TARGET_FILE:client>
          --suite ${suite}
          --report
            ${CMAKE_CURRENT_BINARY_DIR}/test-results/${suite}.json
      )

      set_tests_properties(chat_${suite} PROPERTIES
        TIMEOUT 120
        LABELS ${suite}
      )
    endforeach()
  else()
    message(STATUS "Python 3.8+ not found; chat tests are disabled")
  endif()
endif()
