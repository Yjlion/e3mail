file(GLOB_RECURSE E3CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
if(NOT E3MAIL_P2P)
    list(FILTER E3CORE_SOURCES EXCLUDE REGEX "sync/IrohTransport|engine/Pairing")
endif()

add_library(e3core STATIC ${E3CORE_SOURCES})
target_include_directories(e3core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(e3core
    PUBLIC Qt6::Core Qt6::Network Qt6::Concurrent
    PRIVATE rnp::librnp ${E3MAIL_SQLITE_TARGET} ICU::uc ICU::i18n ICU::data)
if(E3MAIL_P2P)
    target_link_libraries(e3core PRIVATE e3p2p)
    target_compile_definitions(e3core PUBLIC E3MAIL_HAVE_P2P=1)
endif()
if(TARGET Qt6Keychain::Qt6Keychain)
    target_link_libraries(e3core PRIVATE Qt6Keychain::Qt6Keychain)
    target_compile_definitions(e3core PRIVATE E3MAIL_HAVE_KEYCHAIN=1)
endif()
target_compile_definitions(e3core PUBLIC
    E3MAIL_VERSION="${PROJECT_VERSION}")
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/net/OAuth.cpp PROPERTIES COMPILE_DEFINITIONS
    "E3MAIL_OAUTH_GOOGLE_ID=\"${E3MAIL_OAUTH_GOOGLE_ID}\";E3MAIL_OAUTH_GOOGLE_SECRET=\"${E3MAIL_OAUTH_GOOGLE_SECRET}\";E3MAIL_OAUTH_MICROSOFT_ID=\"${E3MAIL_OAUTH_MICROSOFT_ID}\"")
set_target_properties(e3core PROPERTIES AUTOMOC ON)
e3mail_set_warnings(e3core)
