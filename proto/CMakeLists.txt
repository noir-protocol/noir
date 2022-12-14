list(APPEND PROTO_SOURCES
  gogoproto/gogo.proto
  tendermint/abci/types.proto
  tendermint/blocksync/types.proto
  tendermint/consensus/types.proto
  tendermint/consensus/wal.proto
  tendermint/crypto/keys.proto
  tendermint/crypto/proof.proto
  tendermint/libs/bits/types.proto
  tendermint/mempool/types.proto
  tendermint/p2p/conn.proto
  tendermint/p2p/pex.proto
  tendermint/p2p/types.proto
  tendermint/privval/service.proto
  tendermint/privval/types.proto
  tendermint/rpc/grpc/types.proto
  tendermint/state/types.proto
  tendermint/statesync/types.proto
  tendermint/types/block.proto
  tendermint/types/canonical.proto
  tendermint/types/events.proto
  tendermint/types/evidence.proto
  tendermint/types/params.proto
  tendermint/types/types.proto
  tendermint/types/validator.proto
  tendermint/version/types.proto
  )

find_package(Protobuf REQUIRED)

set(PROTO_PATH ${PROJECT_SOURCE_DIR}/proto)
foreach(PROTO ${PROTO_SOURCES})
  set(PROTO_OUTPUT ${PROTO})
  string(REPLACE "\.proto" ".pb.cc" PROTO_OUTPUT ${PROTO_OUTPUT})
  if(NOT EXISTS ${PROTO_OUTPUT})
    execute_process(
      COMMAND protoc --proto_path=${PROTO_PATH} --cpp_out=${PROTO_PATH} ${PROTO_PATH}/${PROTO}
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    list(APPEND PROTO_GENERATED ${PROTO_OUTPUT})
  endif()
endforeach()

add_library(noir_proto STATIC ${PROTO_GENERATED})
target_include_directories(noir_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(noir_proto protobuf::libprotobuf)

add_library(noir::proto ALIAS noir_proto)
