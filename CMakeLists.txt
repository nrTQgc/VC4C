cmake_minimum_required (VERSION 3.1)
include(${CMAKE_ROOT}/Modules/ExternalProject.cmake)

####
# General configuration
####
# Option to enable/disable test-program
option(BUILD_TESTING "Build testing program" ON)
# Option to enable/disable debug-output
option(BUILD_DEBUG "Build with debugging symbols. Otherwise build for performance" ON)
# Option to enable/disable multi-threaded compilation
option(MULTI_THREADED "Runs the optimization steps multi-threaded" ON)
# Option whether to verify the output (using vc4asm)
option(VERIFY_OUTPUT "uses the external library vc4asm to verify the output" ON)
# Option whether to include the LLVM-IR front-end
option(LLVMIR_FRONTEND "Enables the standard frontend for the LLVM intermediate language" ON)
# Option to enable/disable cross compilation
option(CROSS_COMPILE "Cross compile for Raspbian" OFF)
# Option whether to include the SPIR-V front-end
option(SPIRV_FRONTEND "Enables a second frontend for the SPIR-V intermediate language" ON)

# Path to the VC4CL standard library
if(NOT VC4CL_STDLIB_HEADER_SOURCE)
	find_file(VC4CL_STDLIB_FOUND "VC4CLStdLib/include/VC4CLStdLib.h")
	if(VC4CL_STDLIB_FOUND) # Check system path
		set(VC4CL_STDLIB_HEADER_SOURCE "${VC4CL_STDLIB_FOUND}")
	elseif(EXISTS "${CMAKE_SOURCE_DIR}/../VC4CLStdLib/include/VC4CLStdLib.h") # Check local path
		set(VC4CL_STDLIB_HEADER_SOURCE "${CMAKE_SOURCE_DIR}/../VC4CLStdLib/include/VC4CLStdLib.h")
	else()
		message(FATAL_ERROR "VC4CL standard-library headers not found!")
	endif()
	message(STATUS "VC4CL standard library headers found: ${VC4CL_STDLIB_HEADER_SOURCE}")
endif()

if(CROSS_COMPILE)
	if(NOT CROSS_COMPILER_PATH)
		set(CROSS_COMPILER_PATH "/opt/rasperrypi/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64")
	endif()
	message(STATUS "Cross compiling for Raspbian with compiler: ${CROSS_COMPILER_PATH}")
	#Set Cross compiler
	SET(CMAKE_SYSTEM_NAME 		"Linux")
	SET(CMAKE_C_COMPILER   		"${CROSS_COMPILER_PATH}/bin/arm-linux-gnueabihf-gcc")
	SET(CMAKE_CXX_COMPILER 		"${CROSS_COMPILER_PATH}/bin/arm-linux-gnueabihf-g++")
	SET(CMAKE_FIND_ROOT_PATH  	"${CROSS_COMPILER_PATH} ${CROSS_COMPILER_PATH}/arm-linux-gnueabihf")

	# Raspbian ships CLang 3.9 in its repositories
	add_definitions(-DCLANG_PATH="/usr/bin/clang-3.9")
	set(CLANG_FOUND ON)
	add_definitions(-DUSE_CLANG_OPENCL=1)
endif()


project (VC4C VERSION 0.4)
add_definitions(-DVC4C_VERSION="${PROJECT_VERSION}")

# append usage of C++ to compiler flags, also optimize for speed and enable all warnings
if(BUILD_DEBUG)
	LIST(APPEND CMAKE_CXX_FLAGS "-std=c++11 -g3 -rdynamic -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-write-strings")
	SET(CMAKE_BUILD_TYPE 		Debug)
	add_definitions(-DDEBUG_MODE=1)
else()
	LIST(APPEND CMAKE_CXX_FLAGS "-std=c++11 -O3 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-write-strings")
	SET(CMAKE_BUILD_TYPE 		Release)
endif()

if(MULTI_THREADED)
	message(STATUS "Enabling multi-threaded optimizations")
	add_definitions(-DMULTI_THREADED=1)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

# Enable sanitizers
if(BUILD_DEBUG AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER "6.0.0" AND FALSE)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=leak -fsanitize=undefined ")
endif()

####
# Dependencies
####
add_definitions(-DCPPLOG_NAMESPACE=logging)
add_definitions(-DCPPLOG_CUSTOM_LOGGER=true)
ExternalProject_Add( cpplog-project
	PREFIX 				${CMAKE_BINARY_DIR}/build/cpplog
	BINARY_DIR 			${CMAKE_BINARY_DIR}/build/cpplog
	SOURCE_DIR 			${CMAKE_SOURCE_DIR}/lib/cpplog
	GIT_REPOSITORY 		https://github.com/doe300/cpplog.git
	UPDATE_COMMAND 		git pull -f https://github.com/doe300/cpplog.git
	CMAKE_ARGS 			-DCPPLOG_NAMESPACE=logging -DCPPLOG_CUSTOM_LOGGER=true
	STEP_TARGETS 		build	#If we set our dependency on this, the install step is skipped
  	EXCLUDE_FROM_ALL 	TRUE	#Skip for "make all" to skip install
  	TIMEOUT 			30		#Timeout for downloads, in seconds
)

# Prefer Khronos OpenCL to LLVM IR (to SPIR-V) compiler
if(NOT SPIRV_COMPILER_ROOT)
	# Try to detect the location of the SPIR-VLLVM binaries
	find_program(LLVM_SPIRV_FOUND NAMES llvm-spirv HINTS "/opt/SPIRV-LLVM/build/bin/")
	if(LLVM_SPIRV_FOUND)
		get_filename_component(SPIRV_COMPILER_ROOT "${LLVM_SPIRV_FOUND}" DIRECTORY)
	endif()
endif()
if(SPIRV_COMPILER_ROOT)
	message(STATUS "Khronos OpenCL toolkit: ${SPIRV_COMPILER_ROOT}")
	find_file(SPIRV_CLANG_FOUND clang PATHS ${SPIRV_COMPILER_ROOT} NO_DEFAULT_PATH)
	find_file(SPIRV_LLVM_SPIR_FOUND llvm-spirv PATHS ${SPIRV_COMPILER_ROOT} NO_DEFAULT_PATH)
	if(SPIRV_CLANG_FOUND)
		message(STATUS "Khronos OpenCL compiler: ${SPIRV_CLANG_FOUND}")
		add_definitions(-DSPIRV_CLANG_PATH="${SPIRV_CLANG_FOUND}")
	endif()
elseif(SPIRV_FRONTEND)
	message(STATUS "SPIR-V frontend configured, but no SPIR-V compiler found!")
endif()

# fall back to "standard" CLang
if(LLVMIR_FRONTEND AND NOT CLANG_FOUND)
	find_program(CLANG_FOUND clang)
	if(CLANG_FOUND)
		message(STATUS "CLang compiler found: " ${CLANG_FOUND})
		add_definitions(-DCLANG_PATH="${CLANG_FOUND}")
		# check whether CLang is installed in the version 3.9 or greater
		# https://stackoverflow.com/questions/16150888/cmake-branch-on-clang-version
		EXECUTE_PROCESS( COMMAND ${CMAKE_CXX_COMPILER} --version OUTPUT_VARIABLE clang_full_version_string )
		string (REGEX REPLACE ".*clang version ([0-9]+\\.[0-9]+).*" "\\1" CLANG_VERSION_STRING ${clang_full_version_string})
		if (CLANG_VERSION_STRING VERSION_GREATER 3.9)
			message(STATUS "Using enhanced OpenCL features of Clang 3.9+!")
			add_definitions(-DUSE_CLANG_OPENCL=1)
		endif()
	else()
		message(STATUS "No CLang compiler found!")
	endif()
endif()

# If the complete tool collection is provided, compile the SPIR-V frontend
if(SPIRV_LLVM_SPIR_FOUND AND SPIRV_FRONTEND)
	message(STATUS "Compiling SPIR-V front-end...")
	#Add SPIR-V headers project
	ExternalProject_Add(SPIRV-Headers
		PREFIX 				${CMAKE_BINARY_DIR}/build/spirv-headers
		SOURCE_DIR 			${CMAKE_SOURCE_DIR}/lib/spirv-headers
		GIT_REPOSITORY 		https://github.com/KhronosGroup/SPIRV-Headers.git
		UPDATE_COMMAND 		git pull -f https://github.com/KhronosGroup/SPIRV-Headers.git
		STEP_TARGETS 		build
  		EXCLUDE_FROM_ALL	TRUE
  		TIMEOUT 			30		#Timeout for downloads, in seconds
	)
	# skip executables and tests for the SPIR-V parser
	ExternalProject_Add(spirv-tools-project
		DEPENDS 			SPIRV-Headers-build
		PREFIX 				${CMAKE_BINARY_DIR}/build/spirv-tools
		SOURCE_DIR 			${CMAKE_SOURCE_DIR}/lib/spirv-tools
		GIT_REPOSITORY 		https://github.com/KhronosGroup/SPIRV-Tools.git
		UPDATE_COMMAND 		git pull -f https://github.com/KhronosGroup/SPIRV-Tools.git
		CMAKE_ARGS 			-DSPIRV_SKIP_EXECUTABLES:BOOL=ON -DSPIRV_SKIP_TESTS:BOOL=ON -DSPIRV-Headers_SOURCE_DIR:STRING=${CMAKE_CURRENT_SOURCE_DIR}/lib/spirv-headers
		STEP_TARGETS 		build
  		EXCLUDE_FROM_ALL	TRUE
  		TIMEOUT 			30		#Timeout for downloads, in seconds
	)
	add_definitions(-DSPIRV_LLVM_SPIRV_PATH="${SPIRV_LLVM_SPIR_FOUND}")
	add_definitions(-DSPIRV_HEADER="${PROJECT_SOURCE_DIR}/lib/spirv-headers/include/spirv/1.2/spirv.h")
	add_definitions(-DSPIRV_OPENCL_HEADER="${PROJECT_SOURCE_DIR}/lib/spirv-headers/include/spirv/1.2/OpenCL.std.h")
	add_definitions(-DSPIRV_PARSER_HEADER="${PROJECT_SOURCE_DIR}/lib/spirv-tools/include/spirv-tools/libspirv.hpp")
	add_definitions(-DSPIRV_LINKER_HEADER="${PROJECT_SOURCE_DIR}/lib/spirv-tools/include/spirv-tools/linker.hpp")
	include_directories ("${PROJECT_SOURCE_DIR}/lib/spirv-tools/include/")
	# add_definitions(-DSPIRV_OPTIMIZER_HEADER="${PROJECT_SOURCE_DIR}/lib/spirv-tools/include/spirv-tools/optimizer.hpp")
endif()

if(VERIFY_OUTPUT)
	ExternalProject_Add(vc4asm-project
		PREFIX				${CMAKE_BINARY_DIR}/build/vc4asm
		SOURCE_DIR 			${CMAKE_SOURCE_DIR}/lib/vc4asm
		GIT_REPOSITORY 		https://github.com/maazl/vc4asm.git
		UPDATE_COMMAND 		git pull -f https://github.com/maazl/vc4asm.git
		STEP_TARGETS 		build
  		EXCLUDE_FROM_ALL	TRUE
  		TIMEOUT 			30		#Timeout for downloads, in seconds
	)

	add_definitions(-DVERIFIER_HEADER="${PROJECT_SOURCE_DIR}/lib/vc4asm/src/Validator.h")
endif()

# Pre-compile VC4CL standard-library into pre-compiled header
set(VC4CL_STDLIB_HEADER "${PROJECT_SOURCE_DIR}/include/VC4CLStdLib.h.pch")
if(SPIRV_CLANG_FOUND)
	set(PRECOMPILE_COMMAND "${SPIRV_CLANG_FOUND}")
elseif(CLANG_FOUND)
	set(PRECOMPILE_COMMAND "${CLANG_FOUND}")
else()
	message(FATAL_ERROR "No supported OpenCL compiler found!")
endif()
add_custom_target(vc4cl-stdlib
    COMMAND             if [ ! -e ${VC4CL_STDLIB_HEADER} ]; then ${PRECOMPILE_COMMAND} -cc1 -triple spir-unknown-unknown -O3 -cl-std=CL1.2 -cl-kernel-arg-info -cl-single-precision-constant -Wno-all -Wno-gcc-compat -x cl -emit-pch -o ${VC4CL_STDLIB_HEADER} ${VC4CL_STDLIB_HEADER_SOURCE} && echo \"VC4CL standard library precompiled into ${VC4CL_STDLIB_HEADER}\" \; fi
    WORKING_DIRECTORY   ${PROJECT_SOURCE_DIR}
    SOURCES				${VC4CL_STDLIB_HEADER_SOURCE}
)
add_definitions(-DVC4CL_STDLIB_HEADER="${VC4CL_STDLIB_HEADER}")

####
# Main files
####
#build all from ./src into ./build
add_subdirectory(./src build)

if (BUILD_TESTING)
	ExternalProject_Add(cpptest-lite-project
		PREFIX 				${CMAKE_BINARY_DIR}/build/cpptest-lite
		SOURCE_DIR 			${CMAKE_SOURCE_DIR}/lib/cpptest-lite
		GIT_REPOSITORY		https://github.com/doe300/cpptest-lite.git
		UPDATE_COMMAND 		git pull -f https://github.com/doe300/cpptest-lite.git
		STEP_TARGETS 		build
  		EXCLUDE_FROM_ALL	TRUE
  		TIMEOUT 			30		#Timeout for downloads, in seconds
	)
    add_subdirectory(test build/test)
endif (BUILD_TESTING) 