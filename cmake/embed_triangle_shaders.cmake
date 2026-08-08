if(NOT DEFINED GLSLC_EXECUTABLE)
    message(FATAL_ERROR "GLSLC_EXECUTABLE is not set")
endif()
if(NOT DEFINED VERT_SRC)
    message(FATAL_ERROR "VERT_SRC is not set")
endif()
if(NOT DEFINED FRAG_SRC)
    message(FATAL_ERROR "FRAG_SRC is not set")
endif()
if(NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "OUTPUT_HEADER is not set")
endif()

get_filename_component(output_dir "${OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

set(vert_spv "${output_dir}/triangle.vert.spv")
set(frag_spv "${output_dir}/triangle.frag.spv")

execute_process(
    COMMAND "${GLSLC_EXECUTABLE}" "${VERT_SRC}" -o "${vert_spv}"
    RESULT_VARIABLE vert_result
    OUTPUT_VARIABLE vert_stdout
    ERROR_VARIABLE vert_stderr
)
if(NOT vert_result EQUAL 0)
    message(FATAL_ERROR "glslc failed for ${VERT_SRC}: ${vert_stderr}")
endif()

execute_process(
    COMMAND "${GLSLC_EXECUTABLE}" "${FRAG_SRC}" -o "${frag_spv}"
    RESULT_VARIABLE frag_result
    OUTPUT_VARIABLE frag_stdout
    ERROR_VARIABLE frag_stderr
)
if(NOT frag_result EQUAL 0)
    message(FATAL_ERROR "glslc failed for ${FRAG_SRC}: ${frag_stderr}")
endif()

function(spv_to_words spv_path out_var)
    file(READ "${spv_path}" hex HEX)
    string(LENGTH "${hex}" hex_len)
    if(hex_len EQUAL 0)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    math(EXPR byte_count "${hex_len} / 2")
    math(EXPR remainder "${byte_count} % 4")
    if(NOT remainder EQUAL 0)
        message(FATAL_ERROR "SPIR-V size is not a multiple of 4 bytes: ${spv_path}")
    endif()

    math(EXPR word_count "${byte_count} / 4")
    math(EXPR last_index "${word_count} - 1")
    set(words "")
    foreach(i RANGE 0 ${last_index})
        math(EXPR base "${i} * 8")
        math(EXPR b1 "${base} + 2")
        math(EXPR b2 "${base} + 4")
        math(EXPR b3 "${base} + 6")
        string(SUBSTRING "${hex}" ${base} 2 byte0)
        string(SUBSTRING "${hex}" ${b1} 2 byte1)
        string(SUBSTRING "${hex}" ${b2} 2 byte2)
        string(SUBSTRING "${hex}" ${b3} 2 byte3)
        string(APPEND words "    0x${byte3}${byte2}${byte1}${byte0},\n")
    endforeach()

    set(${out_var} "${words}" PARENT_SCOPE)
endfunction()

spv_to_words("${vert_spv}" vert_words)
spv_to_words("${frag_spv}" frag_words)

file(WRITE "${OUTPUT_HEADER}" [==[
#pragma once

#include <cstdint>

namespace evk::assets {

inline constexpr uint32_t triangle_vert_spv[] = {
]==]
)
file(APPEND "${OUTPUT_HEADER}" "${vert_words}")
file(APPEND "${OUTPUT_HEADER}" [==[
};

inline constexpr uint32_t triangle_frag_spv[] = {
]==]
)
file(APPEND "${OUTPUT_HEADER}" "${frag_words}")
file(APPEND "${OUTPUT_HEADER}" [==[
};

} // namespace evk::assets
]==]
)

file(REMOVE "${vert_spv}" "${frag_spv}")
