# Shared warning flags, applied per target with e3mail_set_warnings().
function(e3mail_set_warnings target)
    if(MSVC)
        # C4244/C4267: integer narrowing, which Qt's int and qsizetype APIs make
        # routine; GCC and clang are not asked for it either. C4702:
        # "unreachable code" reported inside Qt's own headers after inlining.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /wd4244 /wd4267 /wd4702)
        if(E3MAIL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            $<$<CXX_COMPILER_ID:GNU>:-Wshadow=local>
            $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshadow-uncaptured-local>)
        if(E3MAIL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE
        QT_NO_KEYWORDS
        QT_NO_URL_CAST_FROM_STRING)
endfunction()
