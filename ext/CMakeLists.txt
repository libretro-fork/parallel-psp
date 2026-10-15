if(UNITTEST)
	set(ARMIPS_REGEXP OFF CACHE BOOL "" FORCE)
	set(ARMIPS_LIBRARY_ONLY ON CACHE BOOL "" FORCE)
	if(WIN32)
		set(ARMIPS_USE_STD_FILESYSTEM ON CACHE BOOL "" FORCE)
	endif()
	add_subdirectory(armips)
endif()

if(NOT USING_GLES2)
	add_subdirectory(glew)
endif()


# This works around a bug in cpu_features when ${CMAKE_BINARY_DIR} != ${CMAKE_CURRENT_BINRARY_DIR}.
# We also disable the list_cpu_features binary.
add_subdirectory(cmake/cpu_features EXCLUDE_FROM_ALL)

add_subdirectory(snappy)
add_subdirectory(udis86)
add_subdirectory(gason)
add_subdirectory(vma)
add_subdirectory(cityhash)
add_subdirectory(basis_universal)
add_subdirectory(libkirk)
add_subdirectory(sfmt19937)
add_subdirectory(xbrz)

add_library(xxhash STATIC
	xxhash.c
	xxhash.h
)

set(aemu_postoffice_client_obj
	aemu_postoffice_client/postoffice.c
	aemu_postoffice_client/postoffice_mem_stdc.c
	aemu_postoffice_client/log_impl_ppsspp.cpp
)

if(WIN32)
	list(APPEND aemu_postoffice_client_obj
		aemu_postoffice_client/sock_impl_windows.c
	)
endif()

if(LINUX OR ANDROID OR MACOSX OR IOS OR BSD)
	list(APPEND aemu_postoffice_client_obj
		aemu_postoffice_client/sock_impl_linux.c
	)
endif()

add_library(aemu_postoffice_client STATIC
	${aemu_postoffice_client_obj}
)
# Its close handshake parks on libretro-common's eventcount.
target_link_libraries(aemu_postoffice_client libretro-common)

set(FT_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_BZIP2 OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_PNG OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_HARFBUZZ OFF CACHE BOOL "" FORCE)
set(FT_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)

if(NOT HTTPS_NOT_AVAILABLE)
	add_subdirectory(naett-build)
endif()

if(ANDROID)
    if (ARM64)
        add_subdirectory(libadrenotools)
    endif()
endif()
