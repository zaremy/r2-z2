cmake_minimum_required(VERSION 3.16)
# board_revision lives in firmware/platform/, not in this app -- it is platform
# code that display and LVGL bring-up will also need. See ../platform/board_revision/PROVENANCE.md
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../platform")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(board_check)
