cmake_minimum_required(VERSION 2.8.3)
PROJECT(indi_dreamfocuser_focus C CXX)

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})
set(CMAKE_CXX_FLAGS "-std=c++0x ${CMAKE_CXX_FLAGS}")

find_package(INDI 1.0 REQUIRED)

if(INDI_FOUND)
  include_directories(${INDI_INCLUDE_DIR})
  add_executable(indi_dreamfocuser_focus dreamfocuser.cpp)
  target_link_libraries(indi_dreamfocuser_focus ${INDI_LIBRARIES})
  install(TARGETS indi_dreamfocuser_focus RUNTIME DESTINATION bin )
  install(FILES indi_dreamfocuser_focus.xml DESTINATION ${INDI_DATA_DIR})
endif(INDI_FOUND)
