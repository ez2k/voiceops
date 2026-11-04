cmake_minimum_required(VERSION 3.22)

# 펌웨어 빌드 번호. CI(web-flasher.yml)가 CALLBRIDGE_BUILD 로 넘겨 주고, 직접 빌드하면 "dev".
# esp_app_get_description()->version 으로 읽혀 부팅 로그와 웹 설정 페이지에 표시된다.
if(DEFINED ENV{CALLBRIDGE_BUILD})
    set(PROJECT_VER "$ENV{CALLBRIDGE_BUILD}")
else()
    set(PROJECT_VER "dev")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(callbridge_standalone)
