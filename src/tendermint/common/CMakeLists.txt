add_library(tendermint_common STATIC lib.cpp)
target_include_directories(tendermint_common PUBLIC ${NOIR_SOURCE_DIR})
target_link_libraries(tendermint_common
  noir::common
  spdlog
)

add_library(tendermint::common ALIAS tendermint_common)

add_noir_test(bitarray_test test/bitarray_test.cpp DEPENDS tendermint_common)
