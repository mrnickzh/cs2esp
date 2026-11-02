cmake_minimum_required(VERSION 3.15)

project(main)

add_compile_options(-fpermissive -mwindows -DUNICODE -D_UNICODE -lgdi32 -luser32 -pthread -static)
add_link_options(-fpermissive -mwindows -DUNICODE -D_UNICODE -lgdi32 -luser32 -pthread -static)

add_executable(main main.cpp)