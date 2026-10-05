add_executable(e3mail-cli main.cpp)
target_link_libraries(e3mail-cli PRIVATE e3core)
e3mail_set_warnings(e3mail-cli)
install(TARGETS e3mail-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
