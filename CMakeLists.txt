cmake_minimum_required(VERSION 3.22)
set(SC_MODULE pdf2txt)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-pdf2txt" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_PDF2TXT_BUILD_TESTS "Build simply-cpp-pdf2txt tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# sc::timer, and sc::params for the program.
find_or_install_package(sc-core simply-cpp-dev simply-cpp 2.0.0)
# The pages' blocks and lines, images left out (image_blocks::left_out, 4.3.0); its shared library, so a
# program also using sc-mupdf has one MuPDF. AGPL-3.0 (MuPDF), so this module is too.
find_or_install_package(sc-mupdf simply-cpp-mupdf-dev simply-cpp-mupdf 4.3.0)

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(pdf2txt
        LINK_LIBRARIES sc::sc-core sc::sc-mupdf-shared
        PUBLIC_LINK_LIBRARIES sc::sc-core sc::sc-mupdf-shared)

add_sc_libraries(DESCRIPTION "simply-cpp PDF text for language models: pages as markdown in reading order, OCR for pages without text")
target_compile_features(sc-pdf2txt PUBLIC cxx_std_20)
target_compile_features(sc-pdf2txt-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-pdf2txt
        COMPONENT development)

if (SC_PDF2TXT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-pdf2txt-test)
endif ()

# pdf2txt, the program: a PDF's text on standard output (no OCR: pages without text are left out).
add_executable(pdf2txt apps/pdf2txt.cpp)
target_link_libraries(pdf2txt PRIVATE sc::sc-pdf2txt-shared sc::sc-core)
target_compile_definitions(pdf2txt PRIVATE SC_PDF2TXT_VERSION="${SC_VERSION}")
sc_program_info(pdf2txt)
install(TARGETS pdf2txt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
sc_man_page(pdf2txt COMPONENT runtime)

package_sc_module(DEPENDS "simply-cpp-mupdf (>= 4.3.0~)" DEV_DEPENDS "simply-cpp-dev (>= 2.0.0~)" "simply-cpp-mupdf-dev (>= 4.3.0~)")
