# common/CMakeLists.txt

add_library(common STATIC
    src/Utils.cpp
    src/Thread_pool.cpp
    src/Logger.cpp
    src/Config.cpp
    src/Task.cpp
    src/Worker.cpp
    src/MessageType.cpp
    src/ProtocolCodec.cpp
    src/Response.cpp
    src/TaskDTO.cpp
    src/WorkerDTO.cpp
    src/protocol/Message_codec.cpp
    src/Buffer.cpp
    src/MetricCounter.cpp
)

# 头文件路径
target_include_directories(common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 编译选项
target_compile_options(common
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

find_package(Threads REQUIRED)
target_link_libraries(common PUBLIC Threads::Threads)

# Prefer the synchronized dependency, including over an old build/deps cache.
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/nlohmann/json.hpp")
    set(GCRP_JSON_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party"
        CACHE PATH "Directory containing nlohmann/json.hpp" FORCE)
else()
    find_path(GCRP_JSON_INCLUDE_DIR nlohmann/json.hpp)
endif()
if(NOT EXISTS "${GCRP_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR "nlohmann/json.hpp is required. Sync third_party/nlohmann/json.hpp or set GCRP_JSON_INCLUDE_DIR.")
endif()
target_include_directories(common SYSTEM PUBLIC ${GCRP_JSON_INCLUDE_DIR})
