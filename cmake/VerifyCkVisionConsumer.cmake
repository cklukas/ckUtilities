cmake_minimum_required(VERSION 3.25)

foreach(_cktools_required
    CKTOOLS_CKVISION_PREFIX
    CKTOOLS_CKVISION_CONSUMER_SOURCE_DIR
    CKTOOLS_CKVISION_CONSUMER_BINARY_DIR
    CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE)
  if(NOT DEFINED ${_cktools_required} OR "${${_cktools_required}}" STREQUAL "")
    message(FATAL_ERROR "${_cktools_required} is required for package verification")
  endif()
endforeach()

set(_cktools_supported_build_types Debug Release RelWithDebInfo MinSizeRel)
if(NOT CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE IN_LIST _cktools_supported_build_types)
  message(FATAL_ERROR
    "Unsupported ckVision package-consumer build type: "
    "${CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE}")
endif()

if(NOT EXISTS "${CKTOOLS_CKVISION_PREFIX}")
  message(FATAL_ERROR "ckVision SDK prefix does not exist: ${CKTOOLS_CKVISION_PREFIX}")
endif()

file(REMOVE_RECURSE "${CKTOOLS_CKVISION_CONSUMER_BINARY_DIR}")

# The external client must use the invoking build's generator/architecture.
# In particular, a Ninja arm64 developer environment must not silently turn
# into the default Visual Studio x64 generator. Match the Windows C runtime
# as well: the installed SDK used by release jobs has the static CRT.
set(_cktools_consumer_configure_args)
if(CKTOOLS_CKVISION_CONSUMER_GENERATOR)
  list(APPEND _cktools_consumer_configure_args
    -G "${CKTOOLS_CKVISION_CONSUMER_GENERATOR}")
endif()
if(CKTOOLS_CKVISION_CONSUMER_GENERATOR_PLATFORM)
  list(APPEND _cktools_consumer_configure_args
    -A "${CKTOOLS_CKVISION_CONSUMER_GENERATOR_PLATFORM}")
endif()
if(CKTOOLS_CKVISION_CONSUMER_GENERATOR_TOOLSET)
  list(APPEND _cktools_consumer_configure_args
    -T "${CKTOOLS_CKVISION_CONSUMER_GENERATOR_TOOLSET}")
endif()
if(CKTOOLS_CKVISION_CONSUMER_MSVC_RUNTIME_LIBRARY)
  list(APPEND _cktools_consumer_configure_args
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CKTOOLS_CKVISION_CONSUMER_MSVC_RUNTIME_LIBRARY}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -S "${CKTOOLS_CKVISION_CONSUMER_SOURCE_DIR}"
    -B "${CKTOOLS_CKVISION_CONSUMER_BINARY_DIR}"
    "-DCMAKE_PREFIX_PATH=${CKTOOLS_CKVISION_PREFIX}"
    "-DCMAKE_BUILD_TYPE=${CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE}"
    ${_cktools_consumer_configure_args}
  RESULT_VARIABLE _cktools_configure_result)
if(NOT _cktools_configure_result EQUAL 0)
  message(FATAL_ERROR "ckVision package consumer configuration failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${CKTOOLS_CKVISION_CONSUMER_BINARY_DIR}"
    --config "${CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE}" --parallel 1
  RESULT_VARIABLE _cktools_build_result)
if(NOT _cktools_build_result EQUAL 0)
  message(FATAL_ERROR "ckVision package consumer build failed")
endif()

execute_process(
  COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CKTOOLS_CKVISION_CONSUMER_BINARY_DIR}"
    -C "${CKTOOLS_CKVISION_CONSUMER_BUILD_TYPE}" --output-on-failure --parallel 1 --no-tests=error
  RESULT_VARIABLE _cktools_test_result)
if(NOT _cktools_test_result EQUAL 0)
  message(FATAL_ERROR "ckVision package consumer test failed")
endif()
