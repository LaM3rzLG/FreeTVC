# SPDX-License-Identifier: GPL-3.0-or-later
# Warning set for freeTVC's own targets; third-party targets keep their defaults.

function(freetvc_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /utf-8
            $<$<COMPILE_LANGUAGE:CXX>:/permissive- /Zc:__cplusplus>
            $<$<BOOL:${FREETVC_WERROR}>:/WX>)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow
            $<$<BOOL:${FREETVC_WERROR}>:-Werror>)
    endif()
endfunction()
