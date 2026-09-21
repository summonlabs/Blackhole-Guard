add_executable(bhg_cli bhg_cli.cpp)
target_link_libraries(bhg_cli PRIVATE BlackholeGuard::blackhole_guard)

install(TARGETS bhg_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
