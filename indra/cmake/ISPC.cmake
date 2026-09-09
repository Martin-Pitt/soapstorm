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

if(DARWIN AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
  set(_default_arch "universal")
  set(_default_targets "neon-i32x4;sse2-i32x4,sse4-i32x4,avx2-i32x8")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" OR (DARWIN AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64"))
  set(_default_arch "aarch64")
  set(_default_targets "neon-i32x4")
else()
  set(_default_arch "x86-64")
  set(_default_targets "sse2-i32x4,sse4-i32x4,avx2-i32x8")
endif()

set(SS_ISPC_ARCH "${_default_arch}" CACHE STRING "ISPC target architecture (x86-64, aarch64, or universal)")
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
#
# On macOS universal builds (arm64 + x86_64), compiles both slices and combines them into a
# universal Mach-O object with lipo so both architecture slices link cleanly.
function(ss_add_ispc_sources out_objects out_include_dir)
  if(NOT SS_ISPC_FOUND)
    message(STATUS "ISPC not configured; Squeeze will use the portable C++ BC7 backend")
    set(${out_objects} "" PARENT_SCOPE)
    set(${out_include_dir} "" PARENT_SCOPE)
    return()
  endif()

  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/ispc_generated")
  file(MAKE_DIRECTORY "${_gen_dir}")

  set(_all_objects "")

  foreach(_src IN LISTS ARGN)
    get_filename_component(_name "${_src}" NAME_WE)
    get_filename_component(_abs "${_src}" ABSOLUTE)

    set(_header "${_gen_dir}/${_name}_ispc.h")
    set(_dispatch_obj "${_gen_dir}/${_name}${CMAKE_C_OUTPUT_EXTENSION}")

    if(DARWIN AND (CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR SS_ISPC_ARCH STREQUAL "universal"))
      # macOS Universal binary build (arm64 + x86_64)
      # 1. Compile arm64 with neon
      # 2. Compile x86_64 with sse2, sse4, avx2
      # 3. Partially link x86_64 objects into a single object with ld -r -arch x86_64
      # 4. Create a universal fat Mach-O object with lipo -create
      set(_arm_obj "${_gen_dir}/${_name}_arm64.o")
      set(_x86_obj "${_gen_dir}/${_name}_x86_64.o")
      set(_x86_sse2 "${_gen_dir}/${_name}_x86_64_sse2.o")
      set(_x86_sse4 "${_gen_dir}/${_name}_x86_64_sse4.o")
      set(_x86_avx2 "${_gen_dir}/${_name}_x86_64_avx2.o")
      set(_x86_combined "${_gen_dir}/${_name}_x86_combined.o")

      set(_outputs "${_dispatch_obj}" "${_header}")

      add_custom_command(
        OUTPUT ${_outputs}
        COMMAND "${SS_ISPC_EXECUTABLE}"
                "${_abs}"
                -o "${_arm_obj}"
                -h "${_header}"
                --target=neon-i32x4
                --arch=aarch64
                --opt=fast-math
                --opt=disable-assertions
                -O2
                --pic
        COMMAND "${SS_ISPC_EXECUTABLE}"
                "${_abs}"
                -o "${_x86_obj}"
                --target=sse2-i32x4,sse4-i32x4,avx2-i32x8
                --arch=x86-64
                --opt=fast-math
                --opt=disable-assertions
                -O2
                --pic
        COMMAND ld -r -arch x86_64 -o "${_x86_combined}" "${_x86_obj}" "${_x86_sse2}" "${_x86_sse4}" "${_x86_avx2}"
        COMMAND lipo -create "${_arm_obj}" "${_x86_combined}" -output "${_dispatch_obj}"
        DEPENDS "${_abs}"
        COMMENT "ISPC universal: ${_name}.ispc -> arm64 (neon) + x86_64 (sse2,sse4,avx2)"
        VERBATIM)

      list(APPEND _all_objects "${_dispatch_obj}")
      set_source_files_properties("${_dispatch_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
      set_source_files_properties("${_header}" PROPERTIES GENERATED TRUE)
    else()
      # Standard single-architecture build (Windows, Linux, or non-universal Darwin)
      string(REPLACE "," ";" _target_list "${SS_ISPC_TARGETS}")
      list(LENGTH _target_list _target_count)
      set(_suffixes "")
      foreach(_t IN LISTS _target_list)
        string(REGEX REPLACE "-.*$" "" _isa "${_t}")
        list(APPEND _suffixes "${_isa}")
      endforeach()

      set(_outputs "${_dispatch_obj}" "${_header}")
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
    endif()
  endforeach()

  set(${out_objects} "${_all_objects}" PARENT_SCOPE)
  set(${out_include_dir} "${_gen_dir}" PARENT_SCOPE)
endfunction()
