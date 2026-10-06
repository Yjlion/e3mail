# Shared warning flags, applied per target with e3mail_set_warnings().
function(e3mail_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
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
