# Blackhole Guard test suites.
#
# Every suite is a plain executable registered with ctest. No test sets or relies on
# a timeout parameter: a hanging test is a defect to be diagnosed, not hidden.

add_library(bhg_test_support STATIC support/testkit.cpp support/process.cpp)
target_include_directories(bhg_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(bhg_test_support PUBLIC BlackholeGuard::blackhole_guard)

function(bhg_add_suite name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE bhg_test_support)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

bhg_add_suite(bhg_tests_core core_test.cpp)
bhg_add_suite(bhg_tests_evidence evidence_test.cpp)
bhg_add_suite(bhg_tests_diagnosis diagnosis_test.cpp)
bhg_add_suite(bhg_tests_authority authority_test.cpp)
bhg_add_suite(bhg_tests_localizer localizer_test.cpp)
bhg_add_suite(bhg_tests_property property_test.cpp)
bhg_add_suite(bhg_tests_protocol protocol_test.cpp)
bhg_add_suite(bhg_tests_persistence persistence_test.cpp)
bhg_add_suite(bhg_tests_restart restart_test.cpp)
bhg_add_suite(bhg_tests_service service_test.cpp)
bhg_add_suite(bhg_tests_concurrency concurrency_test.cpp)
bhg_add_suite(bhg_tests_breakit breakit_test.cpp)
bhg_add_suite(bhg_tests_scale scale_test.cpp)

# Multiprocess proofs: the worker is a real independent executable.
add_executable(bhg_worker multiprocess/worker_main.cpp)
target_link_libraries(bhg_worker PRIVATE bhg_test_support)

bhg_add_suite(bhg_tests_multiprocess multiprocess_test.cpp)
add_dependencies(bhg_tests_multiprocess bhg_worker)
