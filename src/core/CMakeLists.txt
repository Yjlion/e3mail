file(GLOB_RECURSE E3CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

add_library(e3core STATIC ${E3CORE_SOURCES})
target_include_directories(e3core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(e3core
    PUBLIC Qt6::Core Qt6::Network Qt6::Concurrent
    PRIVATE rnp::librnp ${E3MAIL_SQLITE_TARGET})
if(TARGET Qt6Keychain::Qt6Keychain)
    target_link_libraries(e3core PRIVATE Qt6Keychain::Qt6Keychain)
    target_compile_definitions(e3core PRIVATE E3MAIL_HAVE_KEYCHAIN=1)
endif()
target_compile_definitions(e3core PUBLIC
    E3MAIL_VERSION="${PROJECT_VERSION}")
set_target_properties(e3core PROPERTIES AUTOMOC ON)
e3mail_set_warnings(e3core)
