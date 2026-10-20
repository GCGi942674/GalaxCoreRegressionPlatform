# server/CMakeLists.txt

add_library(gcrp_server_core STATIC
    src/Connection.cpp
    src/ConnectionSession.cpp
    src/EchoServer.cpp
    src/EventLoop.cpp
    src/Acceptor.cpp
    src/ServerMetrics.cpp
    src/EchoHandler.cpp
    src/EventLoopThread.cpp
    src/EventLoopThreadPool.cpp
    src/Message.cpp
    src/NetworkWorkerDispatcher.cpp
    src/ProtocolRouter.cpp
    src/GuiCatalog.cpp
    src/RegressionService.cpp
    src/ProgressService.cpp
    src/RunLogArchive.cpp
    src/Scheduler.cpp
    src/SchedulingCoordinator.cpp
    src/SchedulingDriver.cpp
    src/TaskIdGenerator.cpp
    src/TaskService.cpp
    src/StateStore.cpp
    src/WorkerConnectionRegistry.cpp
    src/WorkerManager.cpp
    src/MachineRunPolicy.cpp
    src/WorkerServer.cpp
    src/WorkerSession.cpp
    src/WorkerSessionHandler.cpp
)

target_include_directories(gcrp_server_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(SQLite3 REQUIRED)
target_link_libraries(gcrp_server_core
    PUBLIC
        common
        SQLite::SQLite3
)
add_executable(gcrp_server src/main.cpp)
target_link_libraries(gcrp_server PRIVATE gcrp_server_core)
