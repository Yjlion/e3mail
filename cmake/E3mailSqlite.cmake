# SQLite3 Multiple Ciphers (MIT): SQLite with every page encrypted
# (ChaCha20-Poly1305), so an account's database is unreadable without its key
# (ADR 0023). Fetched at one pinned release and built here, statically, on
# every platform: there is no system SQLite to match, and no OpenSSL to add.
enable_language(C)
include(FetchContent)
FetchContent_Declare(sqlite3mc
    URL https://github.com/utelle/SQLite3MultipleCiphers/releases/download/v2.5.1/sqlite3mc-2.5.1-sqlite-3.53.4-amalgamation.zip
    URL_HASH SHA256=4125f8ff275ea953dabb3289331b20a0e76d4fc060f57148f4a5df3bf3b0d5e0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3mc)
add_library(e3sqlite STATIC ${sqlite3mc_SOURCE_DIR}/sqlite3mc_amalgamation.c)
target_include_directories(e3sqlite SYSTEM PUBLIC ${sqlite3mc_SOURCE_DIR})
target_compile_definitions(e3sqlite
    PUBLIC SQLITE_ENABLE_FTS5=1
    PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DEFAULT_FOREIGN_KEYS=1)
set_target_properties(e3sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
find_package(Threads REQUIRED)
target_link_libraries(e3sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    target_link_libraries(e3sqlite PRIVATE m)
endif()
set(E3MAIL_SQLITE_TARGET e3sqlite)
