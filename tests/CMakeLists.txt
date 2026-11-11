# Shared test helpers: the in-process fake mail system, and an in-process
# network between devices.
add_library(e3testsupport STATIC FakeMailServer.cpp FakeMailServer.h FakeOAuthServer.cpp FakeOAuthServer.h
    FakeCloud.cpp FakeCloud.h
    LoopbackTransport.cpp LoopbackTransport.h DeviceClone.cpp DeviceClone.h)
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
if(NOT E3MAIL_P2P)
    list(FILTER E3_TEST_SOURCES EXCLUDE REGEX "tst_p2p")
endif()
if(NOT E3MAIL_BUILD_APP)
    list(FILTER E3_TEST_SOURCES EXCLUDE REGEX "tst_richtext|tst_notifier|tst_formatting|tst_timegrid")
endif()
if(NOT E3MAIL_CAMERA)
    list(FILTER E3_TEST_SOURCES EXCLUDE REGEX "tst_qrscan")
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

# iCalendar again where the clock is not UTC's: what an event's times mean
# must not depend on where it is read (CI's machines are all in UTC).
# Windows does not take its zone from TZ.
if(NOT WIN32)
    add_test(NAME ical_west COMMAND tst_ical)
    set_tests_properties(ical_west PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=e3.*.info=false;TZ=America/Los_Angeles")
endif()

# Signing in in a browser (src/oauthflow), against FakeOAuthServer.
target_link_libraries(tst_oauth PRIVATE e3oauthflow)
target_compile_definitions(tst_oauth PRIVATE E3MAIL_TEST_BUILT_GOOGLE_ID="${E3MAIL_OAUTH_GOOGLE_ID}")

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
    if(E3MAIL_CAMERA)
        # Codes drawn as the app draws them, read back as the camera reads them.
        target_sources(tst_qrscan PRIVATE ${PROJECT_SOURCE_DIR}/src/app/controllers/QrImageProvider.cpp)
        target_include_directories(tst_qrscan PRIVATE ${PROJECT_SOURCE_DIR}/src/app/controllers)
        target_link_libraries(tst_qrscan PRIVATE e3qrdecode qrcodegen Qt6::Quick)
    endif()
    # The editor's formatter, driven through a real TextArea.
    target_sources(tst_formatting PRIVATE ${PROJECT_SOURCE_DIR}/src/app/controllers/TextFormatter.cpp)
    target_link_libraries(tst_formatting PRIVATE e3richtext Qt6::Quick Qt6::QuickControls2)
    # The calendar's grid of hours, the real TimeGrid.qml, clicked and
    # dragged offscreen. It needs nothing of the app but the theme, so the
    # two files are copied into an import path of the test's own and read
    # from there when it runs. (A QML module built from files outside this
    # directory puts ".." into the names of its generated files, which
    # Windows refuses.)
    set(E3_TIMEGRID_IMPORT ${CMAKE_CURRENT_BINARY_DIR}/timegrid-import)
    foreach(qml TimeGrid Theme)
        configure_file(${PROJECT_SOURCE_DIR}/src/app/qml/${qml}.qml ${E3_TIMEGRID_IMPORT}/E3mail/${qml}.qml COPYONLY)
    endforeach()
    file(WRITE ${E3_TIMEGRID_IMPORT}/E3mail/qmldir "module E3mail\nsingleton Theme 1.0 Theme.qml\nTimeGrid 1.0 TimeGrid.qml\n")
    target_compile_definitions(tst_timegrid PRIVATE E3_TIMEGRID_IMPORT="${E3_TIMEGRID_IMPORT}")
    target_link_libraries(tst_timegrid PRIVATE Qt6::Quick Qt6::QuickControls2)
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
    # A phone: the narrow layout, one pane at a time, both directions.
    add_test(NAME app_smoke_narrow
        COMMAND e3mail --smoke-test --size 400x820 --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-data-narrow)
    add_test(NAME app_smoke_narrow_rtl
        COMMAND e3mail --smoke-test --size 400x820 --lang ar --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-data-narrow-rtl)
    set_tests_properties(app_smoke_narrow app_smoke_narrow_rtl PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;E3MAIL_NO_KEYCHAIN=1"
        FAIL_REGULAR_EXPRESSION "qml: |\\.qml:[0-9]+|ReferenceError|TypeError|Cannot assign|is not a type")
    # A device that last looked at the calendar as a week, or as a day:
    # the view opens that way, with the month never shown. That start once
    # hung the app, so these have a time limit.
    foreach(mode week day)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/smoke-data-${mode}/app.ini "[General]\ncalendarMode=${mode}\n")
    endforeach()
    add_test(NAME app_smoke_week
        COMMAND e3mail --smoke-test --page calendar --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-data-week)
    add_test(NAME app_smoke_day_narrow_rtl
        COMMAND e3mail --smoke-test --page calendar --size 400x820 --lang ar
                --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-data-day)
    set_tests_properties(app_smoke_week app_smoke_day_narrow_rtl PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;E3MAIL_NO_KEYCHAIN=1"
        FAIL_REGULAR_EXPRESSION "qml: |\\.qml:[0-9]+|ReferenceError|TypeError|Cannot assign|is not a type")
endif()
