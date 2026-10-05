# Shared test helpers: the in-process fake mail system.
add_library(e3testsupport STATIC FakeMailServer.cpp FakeMailServer.h)
target_link_libraries(e3testsupport PUBLIC e3core Qt6::Network Qt6::Test)
target_include_directories(e3testsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(e3testsupport PROPERTIES AUTOMOC ON)

# Each tst_<name>.cpp is one Qt Test executable registered with CTest.
function(e3_add_test name)
    add_executable(tst_${name} tst_${name}.cpp ${ARGN})
    target_link_libraries(tst_${name} PRIVATE e3core e3testsupport Qt6::Test)
    target_compile_definitions(tst_${name} PRIVATE
        E3_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    set_target_properties(tst_${name} PROPERTIES AUTOMOC ON)
    e3mail_set_warnings(tst_${name})
    add_test(NAME ${name} COMMAND tst_${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=e3.*.info=false")
endfunction()

file(GLOB E3_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tst_*.cpp)
foreach(src ${E3_TEST_SOURCES})
    get_filename_component(stem ${src} NAME_WE)
    string(REPLACE "tst_" "" name ${stem})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}_support.cpp)
        e3_add_test(${name} ${name}_support.cpp)
    else()
        e3_add_test(${name})
    endif()
endforeach()
