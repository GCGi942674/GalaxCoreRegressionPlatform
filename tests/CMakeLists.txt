# External mirror subset: these are the existing tests, not the missing
# internal GCRP acceptance suite.
add_executable(test_run_log_archive test_run_log_archive.cpp)
add_executable(test_task_groups test_task_groups.cpp)
target_link_libraries(test_task_groups PRIVATE gcrp_server_core)
add_test(NAME test_task_groups COMMAND test_task_groups)
set_tests_properties(test_task_groups PROPERTIES TIMEOUT 90)
target_link_libraries(test_run_log_archive PRIVATE gcrp_server_core)
add_test(NAME test_run_log_archive COMMAND test_run_log_archive)
set_tests_properties(test_run_log_archive PROPERTIES TIMEOUT 30)

message(STATUS "External mirror: building available tests only")
add_executable(test_worker_controls test_worker_controls.cpp)
target_link_libraries(test_worker_controls PRIVATE gcrp_server_core)
add_test(NAME test_worker_controls COMMAND test_worker_controls)
set_tests_properties(test_worker_controls PROPERTIES TIMEOUT 45)
add_executable(test_role_config test_role_config.cpp)
target_link_libraries(test_role_config PRIVATE common)
add_test(NAME test_role_config COMMAND test_role_config)
set_tests_properties(test_role_config PROPERTIES TIMEOUT 15)
foreach(name test_logging test_thread_pool test_EchoClient test_EchoServer)
  add_executable(${name} ${name}.cpp)
  if(name MATCHES "Echo")
    target_sources(${name} PRIVATE ../client/src/EchoClient.cpp)
    target_include_directories(${name} PRIVATE ../client/include)
  endif()
  target_link_libraries(${name} PRIVATE gcrp_server_core)
  target_compile_options(${name} PRIVATE -UNDEBUG)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 45)
endforeach()

# External regressions for the currently synchronized development stage.
foreach(name test_scheduler test_protocol test_worker_network test_server_process test_scheduling_driver test_automatic_scheduling test_dispatch_cancellation test_scheduler_exception_safety test_connection_limits)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE gcrp_server_core)
  add_test(NAME ${name} COMMAND ${name} $<TARGET_FILE:gcrp_server>)
  set_tests_properties(${name} PROPERTIES TIMEOUT 45)
endforeach()
add_dependencies(test_server_process gcrp_server)
target_link_libraries(test_server_process PRIVATE gcrp_client_api)

add_executable(test_control_plane test_control_plane.cpp)
target_link_libraries(test_control_plane PRIVATE gcrp_server_core gcrp_client_api)
add_test(NAME test_control_plane COMMAND test_control_plane)
set_tests_properties(test_control_plane PROPERTIES TIMEOUT 45)

add_executable(test_client_cli test_client_cli.cpp)
target_link_libraries(test_client_cli PRIVATE gcrp_server_core gcrp_client_api)
add_dependencies(test_client_cli gcrp_client)
add_test(NAME test_client_cli COMMAND test_client_cli $<TARGET_FILE:gcrp_client>)
set_tests_properties(test_client_cli PROPERTIES TIMEOUT 45)

add_executable(test_client_transport test_client_transport.cpp)
target_link_libraries(test_client_transport PRIVATE gcrp_client_api)
add_test(NAME test_client_transport COMMAND test_client_transport)
set_tests_properties(test_client_transport PROPERTIES TIMEOUT 45)

find_package(Python3 3.6 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_daily_plan_process COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/fixtures/test_daily_plan_process.py $<TARGET_FILE:gcrp_server>)
set_tests_properties(test_daily_plan_process PROPERTIES TIMEOUT 45)
add_test(NAME test_manager_takeover COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/fixtures/test_manager_takeover.py)
set_tests_properties(test_manager_takeover PROPERTIES TIMEOUT 60)
add_test(NAME test_log_io_process COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/fixtures/test_log_io_process.py $<TARGET_FILE:gcrp_server>)
set_tests_properties(test_log_io_process PROPERTIES TIMEOUT 45)
add_test(NAME test_hardening COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_hardening.py)
set_tests_properties(test_hardening PROPERTIES TIMEOUT 45)
add_test(NAME test_slot_maintenance COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_slot_maintenance.py)
set_tests_properties(test_slot_maintenance PROPERTIES TIMEOUT 30)
add_test(NAME test_worker_control_process COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_worker_control_process.py $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_worker_control_process PROPERTIES TIMEOUT 150)
add_executable(test_python_worker test_python_worker.cpp)
target_link_libraries(test_python_worker PRIVATE gcrp_server_core gcrp_client_api)
add_test(NAME test_python_worker COMMAND test_python_worker ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/worker/gcrp_worker.py)
set_tests_properties(test_python_worker PROPERTIES TIMEOUT 45)

add_executable(test_daily_regression test_daily_regression.cpp)
target_link_libraries(test_daily_regression PRIVATE gcrp_server_core)
add_test(NAME test_daily_regression COMMAND test_daily_regression)
set_tests_properties(test_daily_regression PROPERTIES TIMEOUT 30)

add_executable(test_pjtest_adapter test_pjtest_adapter.cpp)
target_link_libraries(test_pjtest_adapter PRIVATE gcrp_server_core)
add_dependencies(test_pjtest_adapter gcrp_client)
add_test(NAME test_pjtest_adapter COMMAND test_pjtest_adapter
  ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/worker/gcrp_worker.py
  $<TARGET_FILE:gcrp_client> ${PROJECT_SOURCE_DIR}/tests/fixtures/pjtest/setup.py)
set_tests_properties(test_pjtest_adapter PROPERTIES TIMEOUT 60)

add_executable(test_worker_safety test_worker_safety.cpp)
add_test(NAME test_worker_safety COMMAND test_worker_safety
  ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/fixtures/worker_safety.py)
set_tests_properties(test_worker_safety PROPERTIES TIMEOUT 90)

add_executable(test_server_flow test_server_flow.cpp)
target_link_libraries(test_server_flow PRIVATE gcrp_server_core)
add_test(NAME test_server_flow COMMAND test_server_flow)
set_tests_properties(test_server_flow PROPERTIES TIMEOUT 45)

add_executable(test_batch_cancel test_batch_cancel.cpp)
target_link_libraries(test_batch_cancel PRIVATE gcrp_server_core gcrp_client_api)
add_test(NAME test_batch_cancel COMMAND test_batch_cancel)
set_tests_properties(test_batch_cancel PROPERTIES TIMEOUT 45)

add_executable(test_sqlite_persistence test_sqlite_persistence.cpp)
target_link_libraries(test_sqlite_persistence PRIVATE gcrp_server_core)
add_test(NAME test_sqlite_persistence COMMAND test_sqlite_persistence)
set_tests_properties(test_sqlite_persistence PROPERTIES TIMEOUT 45)

add_test(NAME test_clock COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_clock.py)
set_tests_properties(test_clock PROPERTIES TIMEOUT 45)
add_test(NAME test_log_retention COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/fixtures/test_log_retention.py)
add_test(NAME test_config_layout COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_config_layout.py)
set_tests_properties(test_config_layout PROPERTIES TIMEOUT 30)
set_tests_properties(test_log_retention PROPERTIES TIMEOUT 30)


add_test(NAME test_sqlite_worker COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_sqlite_worker.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR}/worker/gcrp_worker.py)
set_tests_properties(test_sqlite_worker PROPERTIES TIMEOUT 45)

add_executable(test_worker_recovery test_worker_recovery.cpp)
target_link_libraries(test_worker_recovery PRIVATE gcrp_server_core)
add_test(NAME test_worker_recovery COMMAND test_worker_recovery)
set_tests_properties(test_worker_recovery PROPERTIES TIMEOUT 45)

add_test(NAME test_recovery_process COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_recovery_process.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR}/worker/gcrp_worker.py)
set_tests_properties(test_recovery_process PROPERTIES TIMEOUT 90)

add_executable(test_worker_distribution test_worker_distribution.cpp)
target_link_libraries(test_worker_distribution PRIVATE gcrp_server_core)
add_test(NAME test_worker_distribution COMMAND test_worker_distribution)
set_tests_properties(test_worker_distribution PROPERTIES TIMEOUT 30)

add_executable(test_regression_service test_regression_service.cpp)
target_link_libraries(test_regression_service PRIVATE gcrp_server_core)
target_compile_options(test_regression_service PRIVATE -UNDEBUG)
add_test(NAME test_regression_service COMMAND test_regression_service)
set_tests_properties(test_regression_service PROPERTIES TIMEOUT 30)

add_executable(test_progress_service test_progress_service.cpp)
target_link_libraries(test_progress_service PRIVATE gcrp_server_core)
add_test(NAME test_progress_service COMMAND test_progress_service)
set_tests_properties(test_progress_service PROPERTIES TIMEOUT 30)

add_test(NAME test_multislot_process COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_multislot_process.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR}/worker/slot_manager.py)
set_tests_properties(test_multislot_process PROPERTIES TIMEOUT 120)

add_test(NAME test_intranet_smoke COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_intranet_smoke.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_intranet_smoke PROPERTIES TIMEOUT 150)

add_test(NAME test_intranet_dual_smoke COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_intranet_dual_smoke.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_intranet_dual_smoke PROPERTIES TIMEOUT 150)

add_test(NAME test_intranet_two_machine_smoke COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_intranet_two_machine_smoke.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_intranet_two_machine_smoke PROPERTIES TIMEOUT 150)

add_test(NAME test_slot_identity COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_slot_identity.py)
set_tests_properties(test_slot_identity PROPERTIES TIMEOUT 20)

add_test(NAME test_intranet_p10_p14 COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/fixtures/test_intranet_p10_p14.py
  $<TARGET_FILE:gcrp_server> ${PROJECT_SOURCE_DIR})
set_tests_properties(test_intranet_p10_p14 PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
add_executable(test_api_queries test_api_queries.cpp)
target_link_libraries(test_api_queries PRIVATE gcrp_server_core)
add_test(NAME test_api_queries COMMAND test_api_queries)
set_tests_properties(test_api_queries PROPERTIES TIMEOUT 30)

# Test assertions must execute in Release too: several legacy tests use assert
add_executable(test_gui_catalog test_gui_catalog.cpp)
target_link_libraries(test_gui_catalog PRIVATE gcrp_server_core)
add_test(NAME test_gui_catalog COMMAND test_gui_catalog)
set_tests_properties(test_gui_catalog PROPERTIES TIMEOUT 30)
if(TARGET gcrp_gui)
  add_test(NAME test_qt_server_catalog COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/tests/fixtures/test_qt_server_catalog.py
    $<TARGET_FILE:gcrp_server> $<TARGET_FILE:gcrp_gui>)
  set_tests_properties(test_qt_server_catalog PROPERTIES TIMEOUT 45)
endif()

add_test(NAME test_service_control COMMAND ${Python3_EXECUTABLE} -B
  ${PROJECT_SOURCE_DIR}/tests/test_service_control.py)
set_tests_properties(test_service_control PROPERTIES TIMEOUT 45)

# Test assertions must execute in Release too: several legacy tests use assert
# for setup operations. Removing those expressions makes the test undefined.
get_directory_property(gcrp_test_targets BUILDSYSTEM_TARGETS)
foreach(gcrp_test_target IN LISTS gcrp_test_targets)
  if(gcrp_test_target MATCHES "^test_")
    if(MSVC)
      target_compile_options(${gcrp_test_target} PRIVATE /UNDEBUG)
    else()
      target_compile_options(${gcrp_test_target} PRIVATE -UNDEBUG)
    endif()
  endif()
endforeach()
