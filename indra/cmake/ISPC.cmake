# -*- cmake -*-
#
# <SS:Nexii> ISPC (Intel Implicit SPMD Program Compiler) support, added for the BC7 encoder
# used by Squeeze. See doc/super_compressed_textures.md.
#
# ISPC is invoked as an explicit custom command rather than through CMake's own ISPC language
# support, because the viewer sets cmake_minimum_required to 3.16 (indra/CMakeLists.txt:13) and
# first-class ISPC support only arrived in 3.19. A custom command also keeps the multi-target
# dispatch build explicit, which is what a block compressor wants: one dispatch object that
# selects at runtime, plus one object per instruction set.
#
# The compiler is NOT vendored. Point SS_ISPC_EXECUTABLE at an install, or leave it unset and
# the build falls back to the portable C++ block backend with no loss of correctness - only of
# encode speed. Nothing here is required to build the viewer.

set(SS_ISPC_EXECUTABLE "" CACHE FILEPATH "Path to ispc.exe. Leave empty to use the portable C++ BC7 backend.")

# Instruction sets to generate. The dispatch object picks the best one the running CPU supports,
# so a build made on an AVX2 machine still runs on an SSE4 one.
#
# SSE2 is in the list as a floor rather than for speed. The default x64 viewer build sets no /arch
# flag at all (indra/cmake/00-Common.cmake:134, "x64 implies SSE2"), so SSE2 is the only instruction
# set the viewer itself actually guarantees. If the running CPU matched none of the generated
# targets the ISPC dispatcher would abort - a hard crash, in a feature whose entire failure story is
# meant to be a silent fall back to uncompressed textures. One extra object is a cheap price for
# that not being possible.
#
# Suffix note for anyone editing this: the object names ispc emits are derived below by stripping
# everything from the first dash, which is correct for these three (sse2, sse4, avx2). Newer ispc
# spells some targets "sse4.1-i32x4", which would derive "sse4.1" while the emitted object is
# "_sse41" - so check the emitted names if you change this list.
set(SS_ISPC_TARGETS "sse2-i32x4,sse4-i32x4,avx2-i32x8" CACHE STRING "ISPC target ISAs, comma separated")

if(SS_ISPC_EXECUTABLE AND EXISTS "${SS_ISPC_EXECUTABLE}")
  set(SS_ISPC_FOUND TRUE)
else()
  set(SS_ISPC_FOUND FALSE)
endif()

# ss_add_ispc_sources(<out_objects_var> <out_include_dir_var> <ispc file> [more ispc files...])
#
# Compiles each .ispc into a dispatch object plus one object per target ISA, and generates the
# C++ header that declares the exported functions. Appends every produced object to
# <out_objects_var> so the caller can add them straight to a target's sources - MSVC and the
# other generators accept .obj files in a source list.
function(ss_add_ispc_sources out_objects out_include_dir)
  if(NOT SS_ISPC_FOUND)
    message(STATUS "ISPC not configured; Squeeze will use the portable C++ BC7 backend")
    set(${out_objects} "" PARENT_SCOPE)
    set(${out_include_dir} "" PARENT_SCOPE)
    return()
  endif()

  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/ispc_generated")
  file(MAKE_DIRECTORY "${_gen_dir}")

  # Turn "sse4-i32x4,avx2-i32x8" into the object-name suffixes ispc actually emits, which are
  # the ISA part only: sse4, avx2.
  string(REPLACE "," ";" _target_list "${SS_ISPC_TARGETS}")
  set(_suffixes "")
  foreach(_t IN LISTS _target_list)
    string(REGEX REPLACE "-.*$" "" _isa "${_t}")
    list(APPEND _suffixes "${_isa}")
  endforeach()

  set(_all_objects "")

  foreach(_src IN LISTS ARGN)
    get_filename_component(_name "${_src}" NAME_WE)
    get_filename_component(_abs "${_src}" ABSOLUTE)

    set(_header "${_gen_dir}/${_name}_ispc.h")
    set(_dispatch_obj "${_gen_dir}/${_name}${CMAKE_C_OUTPUT_EXTENSION}")

    set(_outputs "${_dispatch_obj}" "${_header}")
    foreach(_sfx IN LISTS _suffixes)
      list(APPEND _outputs "${_gen_dir}/${_name}_${_sfx}${CMAKE_C_OUTPUT_EXTENSION}")
    endforeach()

    add_custom_command(
      OUTPUT ${_outputs}
      COMMAND "${SS_ISPC_EXECUTABLE}"
              "${_abs}"
              -o "${_dispatch_obj}"
              -h "${_header}"
              --target=${SS_ISPC_TARGETS}
              --arch=x86-64
              --opt=fast-math
              --opt=disable-assertions
              -O2
              --pic
      DEPENDS "${_abs}"
      COMMENT "ISPC ${_name}.ispc -> ${SS_ISPC_TARGETS}"
      VERBATIM)

    foreach(_o IN LISTS _outputs)
      if(NOT _o MATCHES "\\.h$")
        list(APPEND _all_objects "${_o}")
        set_source_files_properties("${_o}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
      endif()
    endforeach()
    set_source_files_properties("${_header}" PROPERTIES GENERATED TRUE)
  endforeach()

  set(${out_objects} "${_all_objects}" PARENT_SCOPE)
  set(${out_include_dir} "${_gen_dir}" PARENT_SCOPE)
endfunction()
