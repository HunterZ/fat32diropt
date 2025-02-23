cmake_minimum_required(VERSION 3.31.3)
project(fat32diropt VERSION 1.0 LANGUAGES CXX)
add_executable(fat32diropt main.cpp)
set_target_properties(fat32diropt PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF

)
# target_compile_features(fat32diropt PRIVATE cxx_std_17)