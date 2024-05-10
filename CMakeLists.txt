cmake_minimum_required(VERSION 3.5)

project(POKERTH)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/mk/cmake)

set(BUILD_DB ON)
set(BUILD_DBOFFICIAL OFF)
set(BUILD_CLIENT ON)
set(BUILD_PROTOCOL ON)
set(GUI_800_480 OFF)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_AUTOUIC_SEARCH_PATHS};uics/;src/gui/qt;")

if(GUI_800_480)
    add_definitions(-DGUI_800x480)
    set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_AUTOUIC_SEARCH_PATHS};src/gui/qt/gui_800x480;")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FindBoost)
include(FindGsasl)
include(FindTinyXML)

find_package(Boost REQUIRED COMPONENTS iostreams thread filesystem)
find_package(gsasl)
find_package(SQLite3 REQUIRED)
find_package(TinyXML REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(absl REQUIRED CONFIG)

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

if(ANDROID)
    set(POKERTH_LIB_SRC "${POKERTH_LIB_SRC};src/engine/local_engine/tools_android.cpp")
else()
    set(POKERTH_LIB_SRC "${POKERTH_LIB_SRC};src/engine/local_engine/tools.cpp")
endif()

add_library(pokerth_lib "${POKERTH_LIB_SRC}")

target_link_libraries(pokerth_lib PUBLIC "${SQLite3_LIBRARIES}")
target_link_libraries(pokerth_lib PUBLIC "${MYSQLPP_LIBRARIES}")
target_link_libraries(pokerth_lib PUBLIC "${GSASL_LIBRARIES}")
target_link_libraries(pokerth_lib PUBLIC "${Boost_LIBRARIES}")
target_link_libraries(pokerth_lib PUBLIC "${SDLMIXER_LIBRARIES}")
target_link_libraries(pokerth_lib PUBLIC "${TinyXML_LIBRARIES}")
target_link_libraries(pokerth_lib PUBLIC "${OPENSSL_LIBRARIES}")

target_include_directories(pokerth_lib PRIVATE "${Boost_INCLUDE_DIRS}")

target_compile_definitions(pokerth_lib PUBLIC ENABLE_IPV6 TIXML_USE_STL BOOST_FILESYSTEM_DEPRECATED)

target_include_directories(pokerth_lib PUBLIC
    src
    src/engine src/engine/local_engine src/engine/network_engine
    src/gui src/gui/qt src/gui/qt/qttools src/gui/qt/gametable
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

if(BUILD_DB)
    file(GLOB POKERTH_DB_SRC src/db/common/*.cpp)
    add_library(pokerth_db "${POKERTH_DB_SRC}")
    target_include_directories(pokerth_db PRIVATE "${Boost_INCLUDE_DIRS}")
    target_include_directories(pokerth_db PUBLIC src/)
endif()

if(BUILD_PROTOCOL)
    find_package(Protobuf REQUIRED)
    add_library(pokerth_protocol "src/third_party/protobuf/chatcleaner.pb.cc" "src/third_party/protobuf/pokerth.pb.cc")
    target_include_directories(pokerth_protocol PRIVATE "${Protobuf_INCLUDE_DIR}")
    target_include_directories(pokerth_protocol PUBLIC "src/third_party/protobuf")
    target_link_libraries(pokerth_protocol PUBLIC 
        "${Protobuf_LIBRARIES}"
        absl::log_internal_message
        absl::log_internal_check_op)
endif()

if(BUILD_CLIENT)
    find_package(Qt5 REQUIRED COMPONENTS Core Sql Gui Widgets Network REQUIRED)
    find_package(SDL REQUIRED)
    find_package(SDL_mixer REQUIRED)
    find_package(CURL REQUIRED)

    add_library(LibCurl INTERFACE IMPORTED)
    set_target_properties(LibCurl PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIRS}")
    if(WIN32)
      target_link_libraries(LibCurl INTERFACE ${CURL_LIBRARY})
    else()
      set_target_properties(LibCurl PROPERTIES
        INTERFACE_LINK_LIBRARIES "${CURL_LIBRARY}")
    endif()

    file(GLOB POKERTH_CLIENT_SRC src/pokerth.cpp 
        src/gui/qt/chattools/chattools.cpp 
        src/gui/qt/guiwrapper.cpp 
        src/gui/qt/qttools/qttoolswrapper.cpp 
        src/gui/qt/qttools/qthelper/qthelper.cpp 
        src/gui/qt/gametable/gametableimpl.cpp 
        src/gui/qt/gametable/mycardspixmaplabel.cpp 
        src/gui/qt/gametable/mysetlabel.cpp 
        src/gui/qt/gametable/myactionbutton.cpp 
        src/gui/qt/gametable/mystatuslabel.cpp 
        src/gui/qt/gametable/myslider.cpp 
        src/gui/qt/gametable/myavatarlabel.cpp 
        src/gui/qt/gametable/myrighttabwidget.cpp 
        src/gui/qt/gametable/mylefttabwidget.cpp 
        src/gui/qt/gametable/startsplash/startsplash.cpp 
        src/gui/qt/gametable/log/guilog.cpp 
        src/gui/qt/aboutpokerth/aboutpokerthimpl.cpp 
        src/gui/qt/connecttoserverdialog/connecttoserverdialogimpl.cpp 
        src/gui/qt/createnetworkgamedialog/createnetworkgamedialogimpl.cpp 
        src/gui/qt/createinternetgamedialog/createinternetgamedialogimpl.cpp 
        src/gui/qt/joinnetworkgamedialog/joinnetworkgamedialogimpl.cpp 
        src/gui/qt/newlocalgamedialog/newgamedialogimpl.cpp 
        src/gui/qt/settingsdialog/settingsdialogimpl.cpp 
        src/gui/qt/settingsdialog/myavatarbutton.cpp 
        src/gui/qt/settingsdialog/myhpavatarbutton.cpp 
        src/gui/qt/settingsdialog/selectavatardialog/selectavatardialogimpl.cpp 
        src/gui/qt/settingsdialog/selectavatardialog/myavatarlistitem.cpp 
        src/gui/qt/settingsdialog/manualblindsorderdialog/manualblindsorderdialogimpl.cpp 
        src/gui/qt/startnetworkgamedialog/startnetworkgamedialogimpl.cpp 
        src/gui/qt/startwindow/startwindowimpl.cpp 
        src/gui/qt/styles/gametablestylereader.cpp 
        src/gui/qt/styles/carddeckstylereader.cpp 
        src/gui/qt/changecontentdialog/changecontentdialogimpl.cpp 
        src/gui/qt/changecompleteblindsdialog/changecompleteblindsdialogimpl.cpp 
        src/gui/qt/mymessagedialog/mymessagedialogimpl.cpp 
        src/gui/qt/gamelobbydialog/gamelobbydialogimpl.cpp 
        src/gui/qt/gamelobbydialog/mygamelisttreewidget.cpp 
        src/gui/qt/timeoutmsgbox/timeoutmsgboximpl.cpp 
        src/net/common/net_helper_client.cpp 
        src/core/common/loghelper_client.cpp 
        src/gui/qt/gametable/mychancelabel.cpp 
        src/gui/qt/serverlistdialog/serverlistdialogimpl.cpp 
        src/gui/qt/gametable/mytimeoutlabel.cpp 
        src/gui/qt/gametable/mynamelabel.cpp 
        src/gui/qt/settingsdialog/mystylelistitem.cpp 
        src/gui/qt/gamelobbydialog/mygamelistsortfilterproxymodel.cpp 
        src/gui/qt/internetgamelogindialog/internetgamelogindialogimpl.cpp 
        src/engine/local_engine/replay.cpp 
        src/gui/qt/gamelobbydialog/mynicklistsortfilterproxymodel.cpp 
        src/net/common/servermanagerfactoryclient.cpp 
        src/gui/qt/gametable/mycashlabel.cpp 
        src/gui/qt/sound/soundevents.cpp 
        src/gui/qt/sound/sdlplayer.cpp
        src/gui/qt/mymessagebox/mymessagebox.cpp 
        src/gui/qt/logfiledialog/logfiledialog.cpp)

    file(GLOB QTSINGLEAPPLICATION_SRC src/third_party/qtsingleapplication/*.cpp src/third_party/qtlockedfile/qtlockedfile.cpp)

    if(UNIX)
        set(QTSINGLEAPPLICATION_SRC "${QTSINGLEAPPLICATION_SRC};src/third_party/qtlockedfile/qtlockedfile_unix.cpp")
    elseif(WIN32)
        set(QTSINGLEAPPLICATION_SRC "${QTSINGLEAPPLICATION_SRC};src/third_party/qtlockedfile/qtlockedfile_win.cpp")
    endif()

    add_executable(pokerth_client "${POKERTH_CLIENT_SRC}" "${QTSINGLEAPPLICATION_SRC}")
    target_include_directories(pokerth_client PUBLIC src/third_party/qtsingleapplication src/third_party/qtlockedfile)

    target_include_directories(pokerth_client PRIVATE "${Qt5Core_INCLUDE_DIRS}")
    target_include_directories(pokerth_client PRIVATE "${Qt5Sql_INCLUDE_DIRS}")
    target_include_directories(pokerth_client PRIVATE "${Qt5Gui_INCLUDE_DIRS}")
    target_include_directories(pokerth_client PRIVATE "${Qt5Widgets_INCLUDE_DIRS}")
    target_include_directories(pokerth_client PRIVATE "${Boost_INCLUDE_DIRS}")
    target_include_directories(pokerth_client PRIVATE "${SDL_MIXER_INCLUDE_DIRS}")

    target_link_libraries(pokerth_client PUBLIC "${Qt5Core_LIBRARIES}")
    target_link_libraries(pokerth_client PUBLIC "${Qt5Sql_LIBRARIES}")
    target_link_libraries(pokerth_client PUBLIC "${Qt5Gui_LIBRARIES}")
    target_link_libraries(pokerth_client PUBLIC "${Qt5Widgets_LIBRARIES}")
    target_link_libraries(pokerth_client PUBLIC "${Qt5Network_LIBRARIES}")
    target_link_libraries(pokerth_client PUBLIC LibCurl)
    target_link_libraries(pokerth_client PUBLIC "${SDL_LIBRARIES}" "${SDL_MIXER_LIBRARIES}")

    target_link_libraries(pokerth_client PUBLIC pokerth_lib)
    target_link_libraries(pokerth_client PUBLIC pokerth_db)
    target_link_libraries(pokerth_client PUBLIC pokerth_protocol)

    target_include_directories(pokerth_client PUBLIC "uics/")

    target_include_directories(pokerth_client PUBLIC
        src 
        src/engine 
        src/gui 
        src/net 
        src/engine/local_engine 
        src/engine/network_engine 
        src/config 
        src/gui/qt 
        src/gui/qt/connecttoserverdialog 
        src/core 
        src/gui/qt/sound 
        src/gui/qt/qttools 
        src/gui/qt/chattools 
        src/gui/qt/qttools/qthelper 
        src/gui/qt/gametable 
        src/gui/qt/gametable/startsplash 
        src/gui/qt/gametable/log 
        src/gui/qt/aboutpokerth 
        src/gui/qt/createnetworkgamedialog 
        src/gui/qt/createinternetgamedialog 
        src/gui/qt/joinnetworkgamedialog 
        src/gui/qt/newlocalgamedialog 
        src/gui/qt/settingsdialog 
        src/gui/qt/settingsdialog/selectavatardialog 
        src/gui/qt/settingsdialog/manualblindsorderdialog 
        src/gui/qt/startnetworkgamedialog 
        src/gui/qt/startwindow 
        src/gui/qt/serverlistdialog 
        src/gui/qt/styles 
        src/gui/qt/changecontentdialog 
        src/gui/qt/changecompleteblindsdialog 
        src/gui/qt/internetgamelogindialog 
        src/gui/qt/mymessagedialog 
        src/gui/qt/gamelobbydialog 
        src/gui/qt/timeoutmsgbox 
        src/gui/qt/logfiledialog 
        src/gui/qt/mymessagebox/)
endif()