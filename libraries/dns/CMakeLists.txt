include_directories( "${CMAKE_CURRENT_SOURCE_DIR}/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/db/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/blockchain/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/wallet/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/cli/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/rpc/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/client/include" )
include_directories( "${CMAKE_SOURCE_DIR}/libraries/net/include" )

add_library( bts_dns
             #dns_cli.cpp
             dns_rpc_server.cpp
             dns_db.cpp
             dns_transaction_validator.cpp
             dns_wallet.cpp
             outputs.cpp
             util.cpp 
             )

target_link_libraries( bts_dns bts_blockchain bts_db bts_rpc bts_wallet fc leveldb )
