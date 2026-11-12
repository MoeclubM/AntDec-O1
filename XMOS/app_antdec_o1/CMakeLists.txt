cmake_minimum_required(VERSION 3.21)
include($ENV{XMOS_CMAKE_PATH}/xcommon.cmake)
project(app_antdec_o1)

set(APP_HW_TARGET antdec_o1_xu316.xn)
set(APP_INCLUDES src)
set(XMOS_SANDBOX_DIR ${CMAKE_CURRENT_LIST_DIR}/../modules)

set(APP_DEPENDENT_MODULES "lib_xua"
                          "lib_dsp")

set(APP_COMPILER_FLAGS -O3
                       -report
                       -g
                       -Wno-unused-function
                       -DXUD_CORE_CLOCK=600
                       -DXUA_QUAD_SPI_FLASH=1
                       -lquadflash)

XMOS_REGISTER_APP()
