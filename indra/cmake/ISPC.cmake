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

if(NOT SS_ISPC_EXECUTABLE)
  unset(SS_ISPC_EXECUTABLE CACHE)
  find_program(SS_ISPC_EXECUTABLE
    NAMES ispc ispc.exe
    HINTS
      "$ENV{ISPC_DIR}/bin"
      "$ENV{ISPC_DIR}"
      "$ENV{LOCALAPPDATA}/Programs/ispc/bin"
      "C:/tools/ispc/bin"
      "C:/Program Files/ISPC/bin"
    DOC "Path to ispc.exe. Leave empty to use the portable C++ BC7 backend."
  )
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR (DARWIN AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64"))
  set(_default_arch "aarch64")
  set(_default_targets "neon-i32x4")
else()
  set(_default_arch "x86-64")
  set(_default_targets "sse2-i32x4,sse4-i32x4,avx2-i32x8")
endif()

set(SS_ISPC_ARCH "${_default_arch}" CACHE STRING "ISPC target architecture (x86-64 or aarch64)")
set(SS_ISPC_TARGETS "${_default_targets}" CACHE STRING "ISPC target ISAs, comma separated")

if(SS_ISPC_EXECUTABLE AND EXISTS "${SS_ISPC_EXECUTABLE}")
  set(SS_ISPC_FOUND TRUE)
else()
  set(SS_ISPC_FOUND FALSE)
endif()

# ss_add_ispc_sources(<out_objects_var> <out_include_dir_var> <ispc file> [more ispc files...])
#
# Compiles each .ispc into a dispatch object plus one object per target ISA (or a single object
# when targeting a single ISA like NEON), and generates the C++ header that declares the exported
# functions. Appends every produced object to <out_objects_var> so the caller can add them straight
# to a target's sources - MSVC and the other generators accept .obj files in a source list.
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
  list(LENGTH _target_list _target_count)
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
    # When multiple targets are specified, ISPC emits one object per target ISA suffix plus the dispatch object.
    # When only a single target is specified (e.g. neon on ARM64), ISPC emits only the main object without suffixes.
    if(_target_count GREATER 1)
      foreach(_sfx IN LISTS _suffixes)
        list(APPEND _outputs "${_gen_dir}/${_name}_${_sfx}${CMAKE_C_OUTPUT_EXTENSION}")
      endforeach()
    endif()

    add_custom_command(
      OUTPUT ${_outputs}
      COMMAND "${SS_ISPC_EXECUTABLE}"
              "${_abs}"
              -o "${_dispatch_obj}"
              -h "${_header}"
              --target=${SS_ISPC_TARGETS}
              --arch=${SS_ISPC_ARCH}
              --opt=fast-math
              --opt=disable-assertions
              -O2
              --pic
      DEPENDS "${_abs}"
      COMMENT "ISPC ${_name}.ispc -> ${SS_ISPC_TARGETS} (${SS_ISPC_ARCH})"
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
