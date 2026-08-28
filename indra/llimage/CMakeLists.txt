# -*- cmake -*-

project(llimage)

include(00-Common)
include(LLCommon)
include(LLImage)
include(JPEG)
include(LLKDU)
include(ZLIBNG)
include(LLAddBuildTest)
include(bugsplat)
include(Tut)
include(ISPC)   # <SS:Nexii> optional, for the Squeeze BC7 encoder

set(llimage_SOURCE_FILES
    llimagebmp.cpp
    llimage.cpp
    llimagedimensionsinfo.cpp
    llimagedxt.cpp
    llimagefilter.cpp
    llimagej2c.cpp
    llimagejpeg.cpp
    llimagepng.cpp
    llimagetga.cpp
    llimageworker.cpp
    llpngwrapper.cpp
    ssbc7block_mode6.cpp
    ssbc7encoder.cpp
    )

set(llimage_HEADER_FILES
    CMakeLists.txt

    llimage.h
    llimagebmp.h
    llimagedimensionsinfo.h
    llimagedxt.h
    llimagefilter.h
    llimagej2c.h
    llimagejpeg.h
    llimagepng.h
    llimagetga.h
    llimageworker.h
    llmapimagetype.h
    llpngwrapper.h
    ssbc7encoder.h
    )

list(APPEND llimage_SOURCE_FILES ${llimage_HEADER_FILES})

# <SS:Nexii> An ISPC BC7 encoder is used when one is present and ISPC is configured; otherwise the
# portable C++ backend in ssbc7block_mode6.cpp is built instead. Exactly one backend is ever compiled,
# because both define the same three functions behind the seam in ssbc7encoder.h.
set(SS_ISPC_OBJECTS "")
set(SS_ISPC_INCLUDE_DIR "")
if(SS_ISPC_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bc7e/bc7e.ispc")
  ss_add_ispc_sources(SS_ISPC_OBJECTS SS_ISPC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bc7e/bc7e.ispc")
  list(REMOVE_ITEM llimage_SOURCE_FILES ssbc7block_mode6.cpp)
  list(APPEND llimage_SOURCE_FILES ssbc7block_bc7e.cpp)
  message(STATUS "Squeeze: using the ISPC bc7e BC7 backend")
else()
  message(STATUS "Squeeze: using the portable C++ mode 6 BC7 backend")
endif()
# </SS:Nexii>

add_library (llimage ${llimage_SOURCE_FILES} ${SS_ISPC_OBJECTS})

# <SS:Nexii> the ispc generated header lives in the build tree
if(SS_ISPC_INCLUDE_DIR)
  target_include_directories(llimage PRIVATE "${SS_ISPC_INCLUDE_DIR}")
endif()
# </SS:Nexii>
target_include_directories( llimage  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})
# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
if (USE_KDU)
    target_link_libraries(llimage llkdu)
else (USE_KDU)
    target_link_libraries(llimage llimagej2coj)
endif (USE_KDU)

target_link_libraries(llimage
        llfilesystem
        llmath
        llcommon
        ll::libpng
        ll::libjpeg
    )

# Add tests
if (LL_TESTS)
  SET(llimage_TEST_SOURCE_FILES
    llimageworker.cpp
    )
  LL_ADD_PROJECT_UNIT_TESTS(llimage "${llimage_TEST_SOURCE_FILES}")
endif (LL_TESTS)



