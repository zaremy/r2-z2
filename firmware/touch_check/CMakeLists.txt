cmake_minimum_required(VERSION 3.16)
# board_revision lives in firmware/platform/ -- it performs the IO-expander
# sequence that asserts TOUCH_RST, which this app depends on. See
# ../platform/board_revision/PROVENANCE.md
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../platform")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(touch_check)
