cmake_minimum_required(VERSION 3.22)
project(
  groupchat
  VERSION 1.0
  DESCRIPTION "groupchat"
  LANGUAGES C)

include_directories(include)

find_package(Threads REQUIRED)

add_executable(server src/server.c)
add_executable(client src/client.c)

target_link_libraries(client PRIVATE Threads::Threads)
