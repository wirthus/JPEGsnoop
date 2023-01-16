cmake_minimum_required(VERSION 3.7.2)

set(PROJECT_NAME_PREFIX JPEGsnoop)
project(${PROJECT_NAME_PREFIX} VERSION 1.8.2)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

#set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules)

set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
    -Wall \
")

#    -fstrict-aliasing -Wextra -pedantic -Weffc++ -Wfloat-equal -Wswitch-default -Wcast-align -Wcast-qual -Wchar-subscripts -Wcomment \
#    -Wdisabled-optimization -Wformat-nonliteral -Wformat-security -Wconversion -Wformat-nonliteral -Wformat-y2k -Wformat=2 -Wimport \
#    -Winit-self -Winline -Winvalid-pch -Wmissing-format-attribute -Wmissing-include-dirs -Wmissing-noreturn -Wold-style-cast \
#    -Wpacked -Wpointer-arith -Wredundant-decls -Wshadow -Wstack-protector -Wstrict-aliasing=2 -Wswitch-enum -Wunreachable-code \
#    -Wunused -Wvariadic-macros -Wwrite-strings \

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} \
    -DDEBUG \
")

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} \
    -O3 \
")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # using Clang or GNU
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
        -Wall \
        -Wextra \
        -fno-operator-names \
        -Wno-deprecated-declarations \
        -Wno-ignored-qualifiers \
        -Wno-misleading-indentation \
        -Wno-overloaded-virtual \
        -Wno-sign-compare \
        -Wno-trigraphs \
        -Wno-unsequenced \
        -Wno-unused \
        -Wno-unused-but-set-variable \
        -Wno-unused-function \
        -Wno-unused-parameter \
        -Wno-unused-value \
        -Wno-unused-variable \
    ")

    #-Werror \

    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} \
        -g \
    ")
elseif ()
    # using GCC
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
    # using Intel C++
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # using Visual Studio C++

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
        /wd5045 \
        /wd5264 \
        /wd4820 \
        /wd4018 /wd4365 \
    ")
endif ()

find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM)
    # Support Unix Makefiles and Ninja
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
endif ()

set(PROJECT_BUILD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build)
set(PROJECT_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)
set(PROJECT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

find_package(Qt5 COMPONENTS Core REQUIRED)
set(QT_LIBRARIES Qt5::Core)

set(SOURCE_FILES
    src/DecodePs.cpp
    src/General.cpp
    src/ImgDecode.cpp
    src/JfifDecode.cpp
    src/log/ConsoleLog.cpp
    src/main.cpp
    src/Md5.cpp
    src/SnoopConfig.cpp
    src/SnoopCore.cpp
    src/WindowBuf.cpp
    )

set(HEADER_FILES
    src/DecodePs.h
    src/General.h
    src/ImgDecode.h
    src/JfifDecode.h
    src/log/ConsoleLog.h
    src/log/ILog.h
    src/Md5.h
    src/Snoop.h
    src/SnoopConfig.h
    src/SnoopCore.h
    src/WindowBuf.h
    )

add_executable(${PROJECT_NAME} ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(${PROJECT_NAME} PUBLIC ${QT_LIBRARIES})
