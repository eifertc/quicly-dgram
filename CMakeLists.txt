CMAKE_MINIMUM_REQUIRED(VERSION 2.8.11)
CMAKE_POLICY(SET CMP0003 NEW)

PROJECT(quicly)

#FIND_PACKAGE(OpenSSL REQUIRED)
#IF (OPENSSL_FOUND AND (OPENSSL_VERSION VERSION_LESS "1.0.2"))
#    MESSAGE(FATAL "OpenSSL 1.0.2 or above is missing")
#ENDIF ()

SET(CMAKE_C_FLAGS "-std=c99 -Wall -O2 -g ${CC_WARNING_FLAGS} ${CMAKE_C_FLAGS}")

FIND_PACKAGE(PkgConfig REQUIRED)

PKG_CHECK_MODULES(OPENSSL REQUIRED openssl)

IF (OPENSSL_VERSION VERSION_LESS "1.0.2")
  MESSAGE(FATAL "OpenSSL 1.0.2 or above is missing")
ENDIF ()

IF (CMAKE_SYSTEM_NAME STREQUAL "Linux")
     SET(CMAKE_C_FLAGS "-D_GNU_SOURCE -pthread ${CMAKE_C_FLAGS}")
ENDIF ()

#INCLUDE_DIRECTORIES(
#    ${OPENSSL_INCLUDE_DIRS} 
#    deps/klib 
#    deps/picotls/include 
#    deps/picotest include)

INCLUDE(deps/picotls/cmake/dtrace-utils.cmake)

CHECK_DTRACE(${CMAKE_SOURCE_DIR}/deps/picotls/picotls-probes.d)
OPTION(WITH_DTRACE "use USDT (userspace Dtrace probes)" ${HAVE_DTRACE})
IF (WITH_DTRACE)
    MESSAGE(STATUS "Enabling USDT support")
ENDIF ()

INCLUDE_DIRECTORIES(
    ${OPENSSL_INCLUDE_DIR}
    deps/klib
    deps/picotls/include
    deps/picotest include
    ${CMAKE_CURRENT_BINARY_DIR}
)
SET(PICOTLS_OPENSSL_FILES
    deps/picotls/lib/openssl.c
    deps/picotls/lib/pembase64.c
    deps/picotls/lib/picotls.c)

SET(QUICLY_LIBRARY_FILES
    lib/frame.c
    lib/cc-reno.c
    lib/defaults.c
    lib/quicly.c
    lib/ranges.c
    lib/recvstate.c
    lib/sendstate.c
    lib/sentmap.c
    lib/streambuf.c)

SET(UNITTEST_SOURCE_FILES
    deps/picotest/picotest.c
    t/frame.c
    t/maxsender.c
    t/loss.c
    t/ranges.c
    t/sentmap.c
    t/simple.c
    t/stream-concurrency.c
    t/test.c)

SET(GST_SINK_LIBRARY_FILES
    libgstquiclysink/gstquiclysink.c
    libgstquiclysink/config.h
    libgstquiclysink/gstquiclysink.h)

SET(GST_SRC_LIBRARY_FILES
    libgstquiclysrc/gstquiclysrc.c
    libgstquiclysrc/gstquiclysrc.c)

# Add gstreamer libraries
PKG_CHECK_MODULES(GST REQUIRED
            gstreamer-1.0
            gstreamer-base-1.0
            gstreamer-plugins-base-1.0
            gstreamer-plugins-good-1.0
            gstreamer-net-1.0)
message("${GST_CFLAGS}"
)

IF (WITH_DTRACE)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DPICOTLS_USE_DTRACE=1 -DQUICLY_USE_DTRACE=1")
    DEFINE_DTRACE_DEPENDENCIES(${CMAKE_SOURCE_DIR}/deps/picotls/picotls-probes.d picotls)
    DEFINE_DTRACE_DEPENDENCIES(${CMAKE_SOURCE_DIR}/quicly-probes.d quicly)
    LIST(APPEND PICOTLS_OPENSSL_FILES ${CMAKE_CURRENT_BINARY_DIR}/picotls-probes.h)
    LIST(APPEND QUICLY_LIBRARY_FILES ${CMAKE_CURRENT_BINARY_DIR}/quicly-probes.h)
    LIST(APPEND UNITTEST_SOURCE_FILES ${CMAKE_CURRENT_BINARY_DIR}/quicly-probes.h)
    IF (DTRACE_USES_OBJFILE)
        LIST(APPEND PICOTLS_OPENSSL_FILES ${CMAKE_CURRENT_BINARY_DIR}/picotls-probes.o)
        LIST(APPEND QUICLY_LIBRARY_FILES ${CMAKE_CURRENT_BINARY_DIR}/quicly-probes.o)
        LIST(APPEND UNITTEST_SOURCE_FILES ${CMAKE_CURRENT_BINARY_DIR}/quicly-probes.o)
    ENDIF ()
ENDIF ()

ADD_LIBRARY(quicly ${QUICLY_LIBRARY_FILES})
ADD_LIBRARY(quiclydynamic ${QUICLY_LIBRARY_FILES})

# -fPIC flag for the static library linked into the dynamic gstreamer librarys
# Add to compile options
SET(QUICLYDYNAMIC_C_FLAGS "-fpic ${CMAKE_C_FLAGS}")
TARGET_COMPILE_OPTIONS(quiclydynamic
                       PUBLIC
                       "-fPIC"
                       PUBLIC
                       ${CMAKE_C_FLAGS})

LINK_DIRECTORIES(${GST_LIBRARY_DIRS})
ADD_LIBRARY(gstquiclysink MODULE ${PICOTLS_OPENSSL_FILES} ${GST_SINK_LIBRARY_FILES})
TARGET_LINK_LIBRARIES(gstquiclysink 
                      quiclydynamic 
                      ${OPENSSL_LIBRARIES} 
                      ${GST_LIBRARIES})

#TARGET_LINK_OPTIONS(gstquiclysink
#                    PUBLIC
#                    ${GST_LDFLAGS})

TARGET_COMPILE_OPTIONS(gstquiclysink
                       PUBLIC
                       ${GST_CFLAGS})

ADD_LIBRARY(gstquiclysrc MODULE ${PICOTLS_OPENSSL_FILES} ${GST_SRC_LIBRARY_FILES})
TARGET_LINK_LIBRARIES(gstquiclysrc
                      quiclydynamic
                      ${OPENSSL_LIBRARIES}
                      ${GST_LIBRARIES})

#TARGET_LINK_OPTIONS(gstquiclysrc
#		    PUBLIC
#		    ${GST_LDFLAGS})

TARGET_COMPILE_OPTIONS(gstquiclysrc
                       PUBLIC
                       ${GST_CFLAGS})

# gstreamer applications
ADD_EXECUTABLE(quicly_stream gst_src/quicly_stream.c)
TARGET_LINK_LIBRARIES(quicly_stream ${GST_LIBRARIES} "-lgstrtp-1.0")
TARGET_COMPILE_OPTIONS(quicly_stream
                       PUBLIC
                       ${GST_CFLAGS})

ADD_EXECUTABLE(udp_stream gst_src/udp_stream.c)
TARGET_LINK_LIBRARIES(udp_stream ${GST_LIBRARIES})
TARGET_COMPILE_OPTIONS(udp_stream
                       PUBLIC
                       ${GST_CFLAGS})

# oroginal cli compile targets
#ADD_EXECUTABLE(cli ${PICOTLS_OPENSSL_FILES} src/cli.c)
#TARGET_LINK_LIBRARIES(cli quicly ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS})

ADD_CUSTOM_COMMAND(
    OUTPUT embedded-probes.h
    COMMAND ${CMAKE_SOURCE_DIR}/misc/probe2trace.pl -a embedded < ${CMAKE_SOURCE_DIR}/quicly-probes.d > ${CMAKE_CURRENT_BINARY_DIR}/embedded-probes.h
    DEPENDS quicly-probes.d misc/probe2trace.pl
    VERBATIM)

ADD_EXECUTABLE(cli ${PICOTLS_OPENSSL_FILES} ${QUICLY_LIBRARY_FILES} src/cli.c embedded-probes.h)
SET_TARGET_PROPERTIES(cli PROPERTIES COMPILE_FLAGS -DQUICLY_USE_EMBEDDED_PROBES=1)
TARGET_LINK_LIBRARIES(cli ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS})

ADD_EXECUTABLE(test.t ${PICOTLS_OPENSSL_FILES} ${UNITTEST_SOURCE_FILES})
TARGET_LINK_LIBRARIES(test.t quicly ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS})

ADD_EXECUTABLE(examples-echo ${PICOTLS_OPENSSL_FILES} examples/echo.c)
TARGET_LINK_LIBRARIES(examples-echo quicly ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS})

ADD_EXECUTABLE(udpfw t/udpfw.c)

SET_TARGET_PROPERTIES(gstquiclysink
                      PROPERTIES
                      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libgst"
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libgst"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libgst")

SET_TARGET_PROPERTIES(gstquiclysrc
                      PROPERTIES
                      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libgst"
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libgst"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libgst")

ADD_CUSTOM_TARGET(check env BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR} WITH_DTRACE=${WITH_DTRACE} prove --exec "sh -c" -v ${CMAKE_CURRENT_BINARY_DIR}/*.t t/*.t
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS cli test.t)

ADD_CUSTOM_TARGET(format clang-format -i `git ls-files include lib src t | egrep '\\.[ch]$$'`)

# LOG
#get_target_property(MAIN_CFLAGS_SINK gstquiclysink COMPILE_OPTIONS)
#get_target_property(MAIN_CFLAGS_CLI cli COMPILE_OPTIONS)
#message("Target compiler flags for sink: ${MAIN_CFLAGS_SINK}")
#message("Target compiler flags for all: ${CMAKE_C_FLAGS}")

IF (BUILD_FUZZER)
    MESSAGE(STATUS "************* Making the fuzzer")
    IF(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        MESSAGE(FATAL_ERROR "The fuzzer needs clang as a compiler")
    ENDIF()
    ADD_EXECUTABLE(quicly-fuzzer-packet fuzz/packet.cc ${PICOTLS_OPENSSL_FILES})
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_C_FLAGS}")
    IF (OSS_FUZZ)
        # Use https://github.com/google/oss-fuzz compatible options
        SET(LIB_FUZZER FuzzingEngine)
        SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer")
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-omit-frame-pointer")
        TARGET_LINK_LIBRARIES(quicly-fuzzer-packet quicly ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS})
    ELSEIF (USE_CLANG_RT)
        SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fsanitize-coverage=edge,indirect-calls")
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fsanitize-coverage=edge,indirect-calls")
        TARGET_LINK_LIBRARIES(quicly-fuzzer-packet quicly ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS})
    ELSE()
        SET(LIB_FUZZER "${CMAKE_CURRENT_BINARY_DIR}/libFuzzer.a")
        SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-omit-frame-pointer -fsanitize=address -fsanitize-address-use-after-scope -fsanitize=fuzzer-no-link")
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-omit-frame-pointer -fsanitize=address -fsanitize-address-use-after-scope -fsanitize=fuzzer-no-link")
        ADD_CUSTOM_TARGET(libFuzzer ${CMAKE_CURRENT_SOURCE_DIR}/misc/build_libFuzzer.sh WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        ADD_DEPENDENCIES(quicly-fuzzer-packet libFuzzer)
        TARGET_LINK_LIBRARIES(quicly-fuzzer-packet quicly ${OPENSSL_LIBRARIES} ${CMAKE_DL_LIBS} ${LIB_FUZZER})
    ENDIF(OSS_FUZZ)
ENDIF()

