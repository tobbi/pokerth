cmake_minimum_required(VERSION 3.5)

project(POKERTH)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/mk/cmake)

set(BUILD_DBOFFICIAL OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FindBoost)

find_package(Boost)

# POKERTH.lib

file(GLOB POKERTH_LIB_SRC 
    src/engine/game.cpp 
    src/session.cpp 
    src/playerdata.cpp 
    src/config/configfile.cpp 
    src/engine/boardinterface.cpp 
    src/engine/enginefactory.cpp 
    src/engine/handinterface.cpp 
    src/engine/playerinterface.cpp 
    src/engine/berointerface.cpp 
    src/gui/guiinterface.cpp 
    src/core/common/thread.cpp 
    src/core/common/crypthelper.cpp 
    src/core/common/avatarmanager.cpp 
    src/core/common/pokerthexception.cpp 
    src/engine/local_engine/cardsvalue.cpp 
    src/engine/local_engine/localboard.cpp 
    src/engine/local_engine/localenginefactory.cpp 
    src/engine/local_engine/localhand.cpp 
    src/engine/local_engine/localplayer.cpp 
    src/engine/local_engine/localberopreflop.cpp 
    src/engine/local_engine/localberoflop.cpp 
    src/engine/local_engine/localberoturn.cpp 
    src/engine/local_engine/localberoriver.cpp 
    src/engine/local_engine/localberopostriver.cpp 
    src/engine/local_engine/localbero.cpp 
    src/engine/local_engine/localexception.cpp 
    src/engine/local_engine/arraydata.cpp 
    src/engine/log.cpp 
    src/engine/network_engine/clientboard.cpp 
    src/engine/network_engine/clientenginefactory.cpp 
    src/engine/network_engine/clienthand.cpp 
    src/engine/network_engine/clientplayer.cpp 
    src/engine/network_engine/clientbero.cpp 
    src/net/common/chatcleanermanager.cpp 
    src/net/common/chatcleanercallback.cpp 
    src/net/common/clientcallback.cpp 
    src/net/common/clientcontext.cpp 
    src/net/common/clientstate.cpp 
    src/net/common/clientthread.cpp 
    src/net/common/downloadhelper.cpp 
    src/net/common/downloaderthread.cpp 
    src/net/common/netpacket.cpp 
    src/net/common/netpacketvalidator.cpp 
    src/net/common/senderhelper.cpp 
    src/net/common/sendercallback.cpp 
    src/net/common/serverexception.cpp 
    src/net/common/serveracceptinterface.cpp 
    src/net/common/serveracceptwebhelper.cpp 
    src/net/common/servergame.cpp 
    src/net/common/servergamestate.cpp 
    src/net/common/serverlobbythread.cpp 
    src/net/common/serverbanmanager.cpp 
    src/net/common/servercallback.cpp 
    src/net/common/serveradminbot.cpp 
    src/net/common/serverlobbybot.cpp 
    src/net/common/serverircbotcallback.cpp 
    src/net/common/sessiondata.cpp 
    src/net/common/sessiondatacallback.cpp 
    src/net/common/sessionmanager.cpp 
    src/net/common/socket_startup.cpp 
    src/net/common/clientexception.cpp 
    src/net/common/netcontext.cpp 
    src/net/common/netexception.cpp 
    src/net/common/irccallback.cpp 
    src/net/common/servermanager.cpp 
    src/net/common/transferhelper.cpp 
    src/net/common/uploaderthread.cpp 
    src/net/common/uploadhelper.cpp 
    src/gui/generic/serverguiwrapper.cpp 
    src/gui/qttoolsinterface.cpp 
    src/net/common/sendbuffer.cpp 
    src/net/common/asiosendbuffer.cpp 
    src/net/common/websendbuffer.cpp 
    src/net/common/receivebuffer.cpp 
    src/net/common/asioreceivebuffer.cpp 
    src/net/common/webreceivebuffer.cpp 
    src/net/common/uploadcallback.cpp)

add_library(pokerth_lib "${POKERTH_LIB_SRC}")

target_include_directories(pokerth_lib PRIVATE "${Boost_INCLUDE_DIRS}")

target_compile_definitions(pokerth_lib PUBLIC ENABLE_IPV6 TIXML_USE_STL BOOST_FILESYSTEM_DEPRECATED)

target_include_directories(pokerth_lib PUBLIC
    src
    src/engine src/engine/local_engine src/engine/network_engine
    src/gui src/gui/qt src/gui/qt/qttools
    src/net
    src/config
    src/core
    src/third_party/websocketpp)

if(BUILD_DBOFFICIAL)
    file(GLOB POKERTH_DBOFFICIAL_SRC src/dbofficial/*.cpp)

    add_library(pokerth_dbofficial "${POKERTH_DBOFFICIAL_SRC}")
    include(Findmysqlpp)
    find_package(MYSQLPP)
    if(NOT MYSQLPP_FOUND)
        message(FATAL_ERROR "Could not build PokerTH::DBOfficial MySQL++ was not found.")
    endif()
    target_include_directories(pokerth_dbofficial PRIVATE "${Boost_INCLUDE_DIRS}")
    target_include_directories(pokerth_dbofficial PUBLIC src)
endif()