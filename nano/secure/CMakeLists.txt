if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	set (PLATFORM_SECURE_SOURCE plat/osx/working.mm)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
	set (PLATFORM_SECURE_SOURCE plat/windows/working.cpp)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	set (PLATFORM_SECURE_SOURCE plat/posix/working.cpp)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
	set (PLATFORM_SECURE_SOURCE plat/posix/working.cpp)
else ()
	error ("Unknown platform: ${CMAKE_SYSTEM_NAME}")
endif ()

# Embed bootstrap representative weights in executable. Both live
# and beta weights are added to accommodate switching networks
# from the command line.
file(READ ${CMAKE_SOURCE_DIR}/rep_weights_live.bin liveweights HEX)
string(REGEX REPLACE "(..)" "0x\\1," liveweights ${liveweights})
file (WRITE ${CMAKE_BINARY_DIR}/bootstrap_weights_live.cpp "#include <cstddef>\n"
	"namespace nano {\n"
	"	unsigned char nano_bootstrap_weights_live[] = {${liveweights} 0x00};\n"
	"	size_t nano_bootstrap_weights_live_size = sizeof(nano_bootstrap_weights_live) - 1;\n"
	"}\n")

if (EXISTS ${CMAKE_SOURCE_DIR}/rep_weights_beta.bin)
	file(READ ${CMAKE_SOURCE_DIR}/rep_weights_beta.bin betaweights HEX)
	string(REGEX REPLACE "(..)" "0x\\1," betaweights ${betaweights})
endif ()
file (WRITE ${CMAKE_BINARY_DIR}/bootstrap_weights_beta.cpp "#include <cstddef>\n"
	"namespace nano {\n"
	"	unsigned char nano_bootstrap_weights_beta[] = {${betaweights} 0x00};\n"
	"	size_t nano_bootstrap_weights_beta_size = sizeof(nano_bootstrap_weights_beta) - 1;\n"
	"}\n")

# Embed bootstrap priority accounts
if (EXISTS ${CMAKE_SOURCE_DIR}/bootstrap_priority_high_live.bin AND EXISTS ${CMAKE_SOURCE_DIR}/bootstrap_priority_medium_live.bin)
	file(READ ${CMAKE_SOURCE_DIR}/bootstrap_priority_high_live.bin priority_high_live HEX)
	string(REGEX REPLACE "(..)" "0x\\1," priority_high_live ${priority_high_live})
	file(READ ${CMAKE_SOURCE_DIR}/bootstrap_priority_medium_live.bin priority_medium_live HEX)
	string(REGEX REPLACE "(..)" "0x\\1," priority_medium_live ${priority_medium_live})
endif ()
file (WRITE ${CMAKE_BINARY_DIR}/bootstrap_priority_live.cpp "#include <cstddef>\n"
	"namespace nano {\n"
	"	unsigned char nano_bootstrap_priority_high_live[] = {${priority_high_live} 0x00};\n"
	"	size_t nano_bootstrap_priority_high_live_size = sizeof(nano_bootstrap_priority_high_live) - 1;\n"
	"	unsigned char nano_bootstrap_priority_medium_live[] = {${priority_medium_live} 0x00};\n"
	"	size_t nano_bootstrap_priority_medium_live_size = sizeof(nano_bootstrap_priority_medium_live) - 1;\n"
	"}\n")

if (EXISTS ${CMAKE_SOURCE_DIR}/bootstrap_priority_high_beta.bin AND EXISTS ${CMAKE_SOURCE_DIR}/bootstrap_priority_medium_beta.bin)
	file(READ ${CMAKE_SOURCE_DIR}/bootstrap_priority_high_beta.bin priority_high_beta HEX)
	string(REGEX REPLACE "(..)" "0x\\1," priority_high_beta ${priority_high_live})
	file(READ ${CMAKE_SOURCE_DIR}/bootstrap_priority_medium_beta.bin priority_medium_beta HEX)
	string(REGEX REPLACE "(..)" "0x\\1," priority_medium_beta ${priority_medium_live})
endif ()
file (WRITE ${CMAKE_BINARY_DIR}/bootstrap_priority_beta.cpp "#include <cstddef>\n"
	"namespace nano {\n"
	"	unsigned char nano_bootstrap_priority_high_beta[] = {${priority_high_beta} 0x00};\n"
	"	size_t nano_bootstrap_priority_high_beta_size = sizeof(nano_bootstrap_priority_high_beta) - 1;\n"
	"	unsigned char nano_bootstrap_priority_medium_beta[] = {${priority_medium_beta} 0x00};\n"
	"	size_t nano_bootstrap_priority_medium_beta_size = sizeof(nano_bootstrap_priority_medium_beta) - 1;\n"
	"}\n")

add_library (secure
	${PLATFORM_SECURE_SOURCE}
	${CMAKE_BINARY_DIR}/bootstrap_weights_live.cpp
	${CMAKE_BINARY_DIR}/bootstrap_weights_beta.cpp
	${CMAKE_BINARY_DIR}/bootstrap_priority_live.cpp
	${CMAKE_BINARY_DIR}/bootstrap_priority_beta.cpp
	common.hpp
	common.cpp
	blockstore.hpp
	blockstore_partial.hpp
	blockstore.cpp
	ledger.hpp
	ledger.cpp
	utility.hpp
	utility.cpp
	versioning.hpp
	versioning.cpp
	working.hpp)

target_link_libraries(secure
	nano_lib
	ed25519
	crypto_lib
	lmdb
	Boost::boost
	Boost::system
	Boost::filesystem)

target_compile_definitions(secure PUBLIC
	-DQT_NO_KEYWORDS
	-DBOOST_ASIO_HAS_STD_ARRAY=1)
