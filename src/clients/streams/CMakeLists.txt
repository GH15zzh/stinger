include_directories(${CMAKE_BINARY_DIR})

set(_csv_stream_sources
	csv_stream/src/main.cpp
	csv_stream/inc/explore_csv.h
)

add_executable(stinger_csv_stream ${_csv_stream_sources})
target_link_libraries(stinger_csv_stream stinger_net)
target_include_directories(stinger_csv_stream PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/csv_stream/inc)

##############################################################################
