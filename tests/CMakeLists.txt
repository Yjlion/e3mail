# Shared test helpers: the in-process fake mail system, and an in-process
# network between devices.
add_library(e3testsupport STATIC FakeMailServer.cpp FakeMailServer.h
    LoopbackTransport.cpp LoopbackTransport.h)
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
if(NOT E3MAIL_BUILD_APP)
    list(FILTER E3_TEST_SOURCES EXCLUDE REGEX "tst_richtext|tst_notifier")
endif()
foreach(src ${E3_TEST_SOURCES})
    get_filename_component(stem ${src} NAME_WE)
    string(REPLACE "tst_" "" name ${stem})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name}_support.cpp)
        e3_add_test(${name} ${name}_support.cpp)
    else()
        e3_add_test(${name})
    endif()
endforeach()

# Not a test: builds the demonstration mailbox used for screenshots.
add_executable(e3mail-fixtures make_fixtures.cpp)
target_link_libraries(e3mail-fixtures PRIVATE e3core e3testsupport)
e3mail_set_warnings(e3mail-fixtures)

if(E3MAIL_BUILD_APP)
    # Every translation complete, with its placeholders intact.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME translations
            COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/scripts/check-translations.py)
    endif()
    target_link_libraries(tst_richtext PRIVATE e3richtext Qt6::Gui)
    target_link_libraries(tst_notifier PRIVATE e3notifier)
    # Boots the real QML application offscreen against an empty data
    # directory: every QML file must load and every type must resolve.
    add_test(NAME app_smoke
        COMMAND e3mail --smoke-test --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-data)
    set_tests_properties(app_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;E3MAIL_NO_KEYCHAIN=1"
        FAIL_REGULAR_EXPRESSION "qml: |\\.qml:[0-9]+|ReferenceError|TypeError|Cannot assign|is not a type")
    # The same, laid out right to left.
    add_test(NAME app_smoke_rtl
        COMMAND e3mail --smoke-test --lang ar --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-data-rtl)
    set_tests_properties(app_smoke_rtl PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;E3MAIL_NO_KEYCHAIN=1"
        FAIL_REGULAR_EXPRESSION "qml: |\\.qml:[0-9]+|ReferenceError|TypeError|Cannot assign|is not a type")
endif()
