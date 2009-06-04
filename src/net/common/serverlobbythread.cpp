/***************************************************************************
 *   Copyright (C) 2007-2009 by Lothar May                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <net/serverlobbythread.h>
#include <net/servergame.h>
#include <net/serverexception.h>
#include <net/senderhelper.h>
#include <net/sendercallback.h>
#include <net/receiverhelper.h>
#include <net/socket_msg.h>
#include <core/avatarmanager.h>
#include <core/loghelper.h>
#include <core/openssl_wrapper.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <boost/lambda/lambda.hpp>
#include <boost/filesystem.hpp>
#include <boost/bind.hpp>

#define SERVER_MAX_NUM_SESSIONS						512		// Maximum number of idle users in lobby.

#define SERVER_CACHE_CLEANUP_INTERVAL_SEC			86400	// 1 day
#define SERVER_SAVE_STATISTICS_INTERVAL_SEC			60
#define SERVER_CHECK_SESSION_TIMEOUTS_INTERVAL_MSEC	500
#define SERVER_REMOVE_GAME_INTERVAL_MSEC			500
#define SERVER_REMOVE_PLAYER_INTERVAL_MSEC			100
#define SERVER_UPDATE_AVATAR_LOCK_INTERVAL_MSEC		1000
#define SERVER_PROCESS_SEND_INTERVAL_MSEC			10

#define SERVER_INIT_AVATAR_CLIENT_LOCK_SEC			30      // Forbid a client to send an additional avatar.

#define SERVER_INIT_SESSION_TIMEOUT_SEC				20
#define SERVER_TIMEOUT_WARNING_REMAINING_SEC		60
#define SERVER_SESSION_ACTIVITY_TIMEOUT_SEC			1800	// 30 min, MUST be > SERVER_TIMEOUT_WARNING_REMAINING_SEC
#define SERVER_SESSION_FORCED_TIMEOUT_SEC			86400	// 1 day, should be quite large.


#define SERVER_STATISTICS_FILE_NAME					"server_statistics.log"
#define SERVER_STATISTICS_STR_TOTAL_PLAYERS			"TotalNumPlayersLoggedIn"
#define SERVER_STATISTICS_STR_TOTAL_GAMES			"TotalNumGamesCreated"
#define SERVER_STATISTICS_STR_MAX_GAMES				"MaxGamesOpen"
#define SERVER_STATISTICS_STR_MAX_PLAYERS			"MaxPlayersLoggedIn"
#define SERVER_STATISTICS_STR_CUR_GAMES				"CurGamesOpen"
#define SERVER_STATISTICS_STR_CUR_PLAYERS			"CurPlayersLoggedIn"

using namespace std;
using boost::asio::ip::tcp;


class ServerSenderCallback : public SenderCallback, public SessionDataCallback
{
public:
	ServerSenderCallback(ServerLobbyThread &server) : m_server(server) {}
	virtual ~ServerSenderCallback() {}

	virtual void SignalNetError(SessionId /*session*/, int /*errorID*/, int /*osErrorID*/)
	{
		// We just ignore send errors for now, on server side.
		// A serious send error should trigger a read error or a read
		// returning 0 afterwards, and we will handle this error.
	}
	virtual void SignalSessionTerminated(unsigned session)
	{
		m_server.GetSender().SignalSessionTerminated(session);
	}

private:
	ServerLobbyThread &m_server;
};


ServerLobbyThread::ServerLobbyThread(GuiInterface &gui, ConfigFile *playerConfig, AvatarManager &avatarManager,
									 boost::shared_ptr<boost::asio::io_service> ioService)
: m_ioService(ioService), m_timerManager(ioService), m_curBanId(0), m_gui(gui), m_avatarManager(avatarManager),
  m_playerConfig(playerConfig), m_curGameId(0), m_curUniquePlayerId(0), m_curSessionId(INVALID_SESSION + 1),
  m_statDataChanged(false), m_startTime(boost::posix_time::second_clock::local_time())
{
	m_work.reset(new boost::asio::io_service::work(*m_ioService));
	m_senderCallback.reset(new ServerSenderCallback(*this));
	m_sender.reset(new SenderHelper(*m_senderCallback, m_ioService));
	m_receiver.reset(new ReceiverHelper);
}

ServerLobbyThread::~ServerLobbyThread()
{
}

void
ServerLobbyThread::Init(const string &pwd, const string &logDir)
{
	m_password = pwd;
	// Read previous server statistics.
	if (!logDir.empty())
	{
		boost::filesystem::path logPath(logDir);
		if (!logDir.empty())
		{
			logPath /= SERVER_STATISTICS_FILE_NAME;
			m_statisticsFileName = logPath.directory_string();
			ReadStatisticsFile();
		}
	}
}

void
ServerLobbyThread::SignalTermination()
{
	Thread::SignalTermination();
	m_work.reset();
	m_ioService->stop();
}

void
ServerLobbyThread::AddConnection(boost::shared_ptr<tcp::socket> sock)
{
	// Create a random session id.
	// This id can be used to reconnect to the server if the connection was lost.
	//unsigned sessionId;

	// TODO: use randomized method.
	//if(!RAND_bytes((unsigned char *)&sessionId, sizeof(sessionId)))
	//{
	//	RAND_pseudo_bytes((unsigned char *)&sessionId, sizeof(sessionId));
	//}

	// Create a new session.
	boost::shared_ptr<SessionData> sessionData(new SessionData(sock, m_curSessionId++, m_sender, *m_senderCallback));
	m_sessionManager.AddSession(sessionData);

	LOG_VERBOSE("Accepted connection - session #" << sessionData->GetId() << ".");

	bool hasClientIp = false;
	if (m_sessionManager.GetRawSessionCount() <= SERVER_MAX_NUM_SESSIONS)
	{
		boost::system::error_code errCode;
		tcp::endpoint clientEndpoint = sock->remote_endpoint(errCode);
		if (!errCode)
		{
			string ipAddress = clientEndpoint.address().to_string(errCode);
			if (!errCode && !ipAddress.empty())
			{
				sessionData->SetClientAddr(ipAddress);
				hasClientIp = true;
				sock->async_read_some(
					boost::asio::buffer(sessionData->GetReceiveBuffer().recvBuf, RECV_BUF_SIZE),
					boost::bind(
						&ServerLobbyThread::HandleRead,
						this,
						sessionData->GetId(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred));
			}
		}
		if (!hasClientIp)
		{
			// We do not accept sessions if we cannot
			// retrieve the client address.
			SessionError(SessionWrapper(sessionData, boost::shared_ptr<PlayerData>()), ERR_NET_INVALID_SESSION);
		}
	}
	else
	{
		// Server is full.
		// Gracefully close this session.
		SessionError(SessionWrapper(sessionData, boost::shared_ptr<PlayerData>()), ERR_NET_SERVER_FULL);
	}
}

void
ServerLobbyThread::ReAddSession(SessionWrapper session, int reason)
{
	if (session.sessionData.get() && session.playerData.get())
	{
		boost::shared_ptr<NetPacket> packet(new NetPacketRemovedFromGame);
		NetPacketRemovedFromGame::Data removedData;
		removedData.removeReason = reason;
		static_cast<NetPacketRemovedFromGame *>(packet.get())->SetData(removedData);
		session.sessionData->GetSender().Send(session.sessionData, packet);

		HandleReAddedSession(session);
	}
}

void
ServerLobbyThread::MoveSessionToGame(ServerGame &game, SessionWrapper session)
{
	// Remove session from the lobby.
	m_sessionManager.RemoveSession(session.sessionData->GetId());
	// Session is now in game state.
	session.sessionData->SetState(SessionData::Game);
	// Store it in the list of game sessions.
	m_gameSessionManager.AddSession(session);
	// Add session to the game.
	game.AddSession(session);
	// Set the game id of the session.
	session.sessionData->SetGameId(game.GetId());
}

void
ServerLobbyThread::RemoveSessionFromGame(SessionWrapper session)
{
	// Just remove the session. Only for fatal errors.
	CloseSession(session);
	session.sessionData->SetGameId(0);
}

void
ServerLobbyThread::CloseSession(SessionWrapper session)
{
	LOG_VERBOSE("Closing session #" << session.sessionData->GetId() << ".");

	m_sessionManager.RemoveSession(session.sessionData->GetId());
	m_gameSessionManager.RemoveSession(session.sessionData->GetId());

	// Update stats (if needed).
	UpdateStatisticsNumberOfPlayers();
}

void
ServerLobbyThread::ResubscribeLobbyMsg(SessionWrapper session)
{
	InternalResubscribeMsg(session);
}

void
ServerLobbyThread::NotifyPlayerJoinedGame(unsigned gameId, unsigned playerId)
{
	// Send notification to players in lobby.
	boost::shared_ptr<NetPacket> packet(new NetPacketGameListPlayerJoined);
	NetPacketGameListPlayerJoined::Data packetData;
	packetData.gameId = gameId;
	packetData.playerId = playerId;
	static_cast<NetPacketGameListPlayerJoined *>(packet.get())->SetData(packetData);
	m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
}

void
ServerLobbyThread::NotifyPlayerLeftGame(unsigned gameId, unsigned playerId)
{
	// Send notification to players in lobby.
	boost::shared_ptr<NetPacket> packet(new NetPacketGameListPlayerLeft);
	NetPacketGameListPlayerLeft::Data packetData;
	packetData.gameId = gameId;
	packetData.playerId = playerId;
	static_cast<NetPacketGameListPlayerLeft *>(packet.get())->SetData(packetData);
	m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
}

void
ServerLobbyThread::NotifyGameAdminChanged(unsigned gameId, unsigned newAdminPlayerId)
{
	// Send notification to players in lobby.
	boost::shared_ptr<NetPacket> packet(new NetPacketGameListAdminChanged);
	NetPacketGameListAdminChanged::Data packetData;
	packetData.gameId = gameId;
	packetData.newAdminplayerId = newAdminPlayerId;
	static_cast<NetPacketGameListAdminChanged *>(packet.get())->SetData(packetData);
	m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
}

void
ServerLobbyThread::NotifyStartingGame(unsigned gameId)
{
	boost::shared_ptr<NetPacket> packet = CreateNetPacketGameListUpdate(gameId, GAME_MODE_STARTED);
	m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
}

void
ServerLobbyThread::NotifyReopeningGame(unsigned gameId)
{
	boost::shared_ptr<NetPacket> packet = CreateNetPacketGameListUpdate(gameId, GAME_MODE_CREATED);
	m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
}

void
ServerLobbyThread::HandleGameRetrievePlayerInfo(SessionWrapper session, const NetPacketRetrievePlayerInfo &tmpPacket)
{
	// Someone within a game requested player info.
	HandleNetPacketRetrievePlayerInfo(session, tmpPacket);
}

void
ServerLobbyThread::HandleGameRetrieveAvatar(SessionWrapper session, const NetPacketRetrieveAvatar &tmpPacket)
{
	// Someone within a game requested an avatar.
	HandleNetPacketRetrieveAvatar(session, tmpPacket);
}

bool
ServerLobbyThread::KickPlayerByName(const std::string &playerName)
{
	bool retVal = false;
	SessionWrapper session = m_sessionManager.GetSessionByPlayerName(playerName);
	if (!session.sessionData.get())
		session = m_gameSessionManager.GetSessionByPlayerName(playerName);

	if (session.sessionData.get() && session.playerData.get())
	{
		RemovePlayer(session.playerData->GetUniqueId(), ERR_NET_PLAYER_KICKED);
		retVal = true;
	}

	return retVal;
}

void
ServerLobbyThread::BanPlayerRegex(const string &playerRegex)
{
	boost::mutex::scoped_lock lock(m_banMutex);
	m_banPlayerNameMap[++m_curBanId] = boost::regex(playerRegex, boost::regex_constants::no_except);
}

void
ServerLobbyThread::BanIPAddress(const string &ipAddress)
{
	boost::mutex::scoped_lock lock(m_banMutex);
	m_banIPAddressMap[++m_curBanId] = ipAddress;
}

bool
ServerLobbyThread::UnBan(unsigned banId)
{
	bool retVal = false;
	boost::mutex::scoped_lock lock(m_banMutex);
	RegexMap::iterator posNick = m_banPlayerNameMap.find(banId);
	if (posNick != m_banPlayerNameMap.end())
	{
		m_banPlayerNameMap.erase(posNick);
		retVal = true;
	}
	else
	{
		IPAddressMap::iterator posIP = m_banIPAddressMap.find(banId);
		if (posIP != m_banIPAddressMap.end())
		{
			m_banIPAddressMap.erase(posIP);
			retVal = true;
		}
	}
	return retVal;
}

void
ServerLobbyThread::GetBanList(list<string> &list) const
{
	boost::mutex::scoped_lock lock(m_banMutex);
	RegexMap::const_iterator i_nick = m_banPlayerNameMap.begin();
	RegexMap::const_iterator end_nick = m_banPlayerNameMap.end();
	while (i_nick != end_nick)
	{
		ostringstream banText;
		banText << (*i_nick).first << ": (nick) - " << (*i_nick).second.str();
		list.push_back(banText.str());
		++i_nick;
	}
	IPAddressMap::const_iterator i_ip = m_banIPAddressMap.begin();
	IPAddressMap::const_iterator end_ip = m_banIPAddressMap.end();
	while (i_ip != end_ip)
	{
		ostringstream banText;
		banText << (*i_ip).first << ": (IP) - " << (*i_ip).second;
		list.push_back(banText.str());
		++i_ip;
	}
}

void
ServerLobbyThread::ClearBanList()
{
	boost::mutex::scoped_lock lock(m_banMutex);
	m_banPlayerNameMap.clear();
	m_banIPAddressMap.clear();
}

string
ServerLobbyThread::GetPlayerIPAddress(const std::string &playerName) const
{
	string ipAddress;
	SessionWrapper session = m_sessionManager.GetSessionByPlayerName(playerName);
	if (!session.sessionData.get())
		session = m_gameSessionManager.GetSessionByPlayerName(playerName);

	if (session.sessionData.get() && session.playerData.get())
		ipAddress = session.sessionData->GetClientAddr();

	return ipAddress;
}

void
ServerLobbyThread::RemovePlayer(unsigned playerId, unsigned errorCode)
{
	boost::mutex::scoped_lock lock(m_removePlayerListMutex);
	m_removePlayerList.push_back(RemovePlayerList::value_type(playerId, errorCode));
}

void
ServerLobbyThread::SendGlobalChat(const string &message)
{
	boost::shared_ptr<NetPacket> outChat(new NetPacketChatText);
	NetPacketChatText::Data outChatData;
	outChatData.playerId = 0;
	outChatData.text = message;
	static_cast<NetPacketChatText *>(outChat.get())->SetData(outChatData);
	m_gameSessionManager.SendToAllSessions(outChat, SessionData::Game);
}

void
ServerLobbyThread::SendGlobalMsgBox(const string &message)
{
	boost::shared_ptr<NetPacket> outMsg(new NetPacketMsgBoxText);
	NetPacketMsgBoxText::Data outMsgData;
	outMsgData.text = message;
	static_cast<NetPacketMsgBoxText *>(outMsg.get())->SetData(outMsgData);
	m_gameSessionManager.SendToAllSessions(outMsg, SessionData::Game);
}

void
ServerLobbyThread::AddComputerPlayer(boost::shared_ptr<PlayerData> player)
{
	boost::mutex::scoped_lock lock(m_computerPlayersMutex);
	m_computerPlayers.insert(PlayerDataMap::value_type(player->GetUniqueId(), player));
}

void
ServerLobbyThread::RemoveComputerPlayer(boost::shared_ptr<PlayerData> player)
{
	boost::mutex::scoped_lock lock(m_computerPlayersMutex);
	m_computerPlayers.erase(player->GetUniqueId());
}

TimerManager &
ServerLobbyThread::GetTimerManager()
{
	return m_timerManager;
}

AvatarManager &
ServerLobbyThread::GetAvatarManager()
{
	return m_avatarManager;
}

ServerStats
ServerLobbyThread::GetStats() const
{
	boost::mutex::scoped_lock lock(m_statMutex);
	return m_statData;
}

boost::posix_time::ptime
ServerLobbyThread::GetStartTime() const
{
	return m_startTime;
}

SenderInterface &
ServerLobbyThread::GetSender()
{
	assert(m_sender);
	return *m_sender;
}

u_int32_t
ServerLobbyThread::GetNextUniquePlayerId()
{
	boost::mutex::scoped_lock lock(m_curUniquePlayerIdMutex);
	m_curUniquePlayerId++;
	if (m_curUniquePlayerId == 0) // 0 is an invalid id.
		m_curUniquePlayerId++;

	return m_curUniquePlayerId;
}

u_int32_t
ServerLobbyThread::GetNextGameId()
{
	m_curGameId++;
	if (m_curGameId == 0) // 0 is an invalid id.
		m_curGameId++;

	return m_curGameId;
}

void
ServerLobbyThread::Main()
{
	try
	{
		// Register all timers.
		RegisterTimers();

		m_ioService->run();
	} catch (const PokerTHException &e)
	{
		GetCallback().SignalNetServerError(e.GetErrorId(), e.GetOsErrorCode());
		LOG_ERROR(e.what());
	}
}

void
ServerLobbyThread::RegisterTimers()
{
	// Remove closed games.
	m_timerManager.RegisterTimer(
		SERVER_REMOVE_GAME_INTERVAL_MSEC,
		boost::bind(&ServerLobbyThread::TimerRemoveGame, this),
		true);
	// Remove inactive/kicked players.
	m_timerManager.RegisterTimer(
		SERVER_REMOVE_PLAYER_INTERVAL_MSEC,
		boost::bind(&ServerLobbyThread::TimerRemovePlayer, this),
		true);
	// Check the timeout of sessions which have not been initialised.
	m_timerManager.RegisterTimer(
		SERVER_CHECK_SESSION_TIMEOUTS_INTERVAL_MSEC,
		boost::bind(&ServerLobbyThread::TimerCheckSessionTimeouts, this),
		true);
	// Cleanup the avatar cache. Note: Only works if there are no users on the server.
	m_timerManager.RegisterTimer(
		SERVER_CACHE_CLEANUP_INTERVAL_SEC * 1000,
		boost::bind(&ServerLobbyThread::TimerCleanupAvatarCache, this),
		true);
	// Update the statistics file.
	m_timerManager.RegisterTimer(
		SERVER_SAVE_STATISTICS_INTERVAL_SEC * 1000,
		boost::bind(&ServerLobbyThread::TimerSaveStatisticsFile, this),
		true);
	// Update the avatar upload locks.
	m_timerManager.RegisterTimer(
		SERVER_UPDATE_AVATAR_LOCK_INTERVAL_MSEC,
		boost::bind(&ServerLobbyThread::TimerUpdateClientAvatarLock, this),
		true);
	// Check if new data needs to be sent.
	m_timerManager.RegisterTimer(
		SERVER_PROCESS_SEND_INTERVAL_MSEC,
		boost::bind(&SenderInterface::Process, m_sender),
		true);
}

void
ServerLobbyThread::HandleRead(SessionId sessionId, const boost::system::error_code& error, size_t bytesRead)
{
	// Find the session.
	SessionWrapper session = m_sessionManager.GetSessionById(sessionId);
	if (!session.sessionData)
		session = m_gameSessionManager.GetSessionById(sessionId);
	if (session.sessionData)
	{
		if (!error)
		{
			ReceiveBuffer &buf = session.sessionData->GetReceiveBuffer();
			buf.recvBufUsed += bytesRead;
			GetReceiver().ScanPackets(buf);

			while (!buf.receivedPackets.empty())
			{
				boost::shared_ptr<NetPacket> packet = buf.receivedPackets.front();
				buf.receivedPackets.pop_front();
				// Retrieve current game, if applicable.
				boost::shared_ptr<ServerGame> game = InternalGetGameFromId(session.sessionData->GetGameId());
				if (game)
				{
					// We need to catch game-specific exceptions, so that they do not affect the server.
					try
					{
						game->HandlePacket(session, packet);
					} catch (const PokerTHException &e)
					{
						LOG_ERROR(e.what());
						InternalRemoveGame(game);
					}
				}
				else
					HandlePacket(session, packet);
			}
			session.sessionData->GetAsioSocket()->async_read_some(
				boost::asio::buffer(buf.recvBuf + buf.recvBufUsed, RECV_BUF_SIZE - buf.recvBufUsed),
				boost::bind(
					&ServerLobbyThread::HandleRead,
					this,
					sessionId,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred));
		}
		else
		{
			// On error: Close this session.
			boost::shared_ptr<ServerGame> game = InternalGetGameFromId(session.sessionData->GetGameId());
			if (game)
				game->ErrorRemoveSession(session);
			else
				CloseSession(session);
		}
	}
}

void
ServerLobbyThread::HandlePacket(SessionWrapper session, boost::shared_ptr<NetPacket> packet)
{
	if (session.sessionData && packet)
	{
		if (packet->IsClientActivity())
			session.sessionData->ResetActivityTimer();

		if (session.sessionData->GetState() == SessionData::Init)
		{
			if (packet->ToNetPacketInit())
				HandleNetPacketInit(session, *packet->ToNetPacketInit());
			else if (packet->ToNetPacketAvatarHeader())
				HandleNetPacketAvatarHeader(session, *packet->ToNetPacketAvatarHeader());
			else if (packet->ToNetPacketUnknownAvatar())
				HandleNetPacketUnknownAvatar(session, *packet->ToNetPacketUnknownAvatar());
			else
				SessionError(session, ERR_SOCK_INVALID_STATE);
		}
		else if (session.sessionData->GetState() == SessionData::ReceivingAvatar)
		{
			if (packet->ToNetPacketAvatarFile())
				HandleNetPacketAvatarFile(session, *packet->ToNetPacketAvatarFile());
			else if (packet->ToNetPacketAvatarEnd())
				HandleNetPacketAvatarEnd(session, *packet->ToNetPacketAvatarEnd());
			else
				SessionError(session, ERR_SOCK_INVALID_STATE);
		}
		else
		{
			if (packet->ToNetPacketRetrievePlayerInfo())
				HandleNetPacketRetrievePlayerInfo(session, *packet->ToNetPacketRetrievePlayerInfo());
			else if (packet->ToNetPacketRetrieveAvatar())
				HandleNetPacketRetrieveAvatar(session, *packet->ToNetPacketRetrieveAvatar());
			else if (packet->ToNetPacketResetTimeout())
			{}
			else if (packet->ToNetPacketUnsubscribeGameList())
				session.sessionData->ResetWantsLobbyMsg();
			else if (packet->ToNetPacketResubscribeGameList())
				InternalResubscribeMsg(session);
			else if (packet->ToNetPacketCreateGame())
				HandleNetPacketCreateGame(session, *packet->ToNetPacketCreateGame());
			else if (packet->ToNetPacketJoinGame())
				HandleNetPacketJoinGame(session, *packet->ToNetPacketJoinGame());
			else
				SessionError(session, ERR_SOCK_INVALID_STATE);
		}
	}
}

void
ServerLobbyThread::HandleNetPacketInit(SessionWrapper session, const NetPacketInit &tmpPacket)
{
	LOG_VERBOSE("Received init for session #" << session.sessionData->GetId() << ".");

	NetPacketInit::Data initData;
	tmpPacket.GetData(initData);

	// Check the protocol version.
	if (initData.versionMajor != NET_VERSION_MAJOR)
	{
		SessionError(session, ERR_NET_VERSION_NOT_SUPPORTED);
		return;
	}
	if (initData.versionMajor == 5 && (initData.versionMinor == 0 || initData.versionMinor == 1))
		session.sessionData->SetMaxNumPlayers(7);
	else
		session.sessionData->SetMaxNumPlayers(MAX_NUMBER_OF_PLAYERS);

	// Check the server password.
	if (!CheckPassword(initData.password))
	{
		SessionError(session, ERR_NET_INVALID_PASSWORD);
		return;
	}

	// Check whether the player name is correct.
	// Partly, this is also done in netpacket.
	// However, some disallowed names are checked only here.
	if (initData.playerName.empty() || initData.playerName.size() > MAX_NAME_SIZE
		|| initData.playerName[0] == '#'
		|| initData.playerName[0] == ' '
		|| initData.playerName.substr(0, sizeof(SERVER_COMPUTER_PLAYER_NAME) - 1) == SERVER_COMPUTER_PLAYER_NAME)
	{
		SessionError(session, ERR_NET_INVALID_PLAYER_NAME);
		return;
	}

	// Check whether this player is already connected.
	if (IsPlayerConnected(initData.playerName))
	{
		SessionError(session, ERR_NET_PLAYER_NAME_IN_USE);
		return;
	}

	// Check whether the player name is banned.
	if (IsPlayerBanned(initData.playerName))
	{
		SessionError(session, ERR_NET_PLAYER_BANNED);
		return;
	}
	// Check whether the peer IP address is banned.
	if (IsIPAddressBanned(session.sessionData->GetClientAddr()))
	{
		SessionError(session, ERR_NET_PLAYER_BANNED);
		return;
	}

	// Create player data object.
	boost::shared_ptr<PlayerData> tmpPlayerData(
		new PlayerData(GetNextUniquePlayerId(), 0, PLAYER_TYPE_HUMAN, PLAYER_RIGHTS_NORMAL));
	tmpPlayerData->SetName(initData.playerName);
	tmpPlayerData->SetNetSessionData(session.sessionData);
	if (initData.showAvatar)
		tmpPlayerData->SetAvatarMD5(initData.avatar);

	// Set player data for session.
	m_sessionManager.SetSessionPlayerData(session.sessionData->GetId(), tmpPlayerData);
	session.playerData = tmpPlayerData;

	string avatarFileName;
	if (initData.showAvatar
		&& !initData.avatar.IsZero()
		&& !GetAvatarManager().GetAvatarFileName(initData.avatar, avatarFileName))
	{
		bool avatarRecentlyRequested = false;
		{
			boost::mutex::scoped_lock lock(m_timerAvatarClientAddressMapMutex);
			if (m_timerAvatarClientAddressMap.find(session.sessionData->GetClientAddr()) != m_timerAvatarClientAddressMap.end())
				avatarRecentlyRequested = true;
		}
		if (avatarRecentlyRequested)
			SessionError(session, ERR_NET_AVATAR_UPLOAD_BLOCKED);
		else
			RequestPlayerAvatar(session);
	}
	else
	{
		if (!avatarFileName.empty())
			session.playerData->SetAvatarFile(avatarFileName);
		EstablishSession(session);
	}
}

void
ServerLobbyThread::HandleNetPacketAvatarHeader(SessionWrapper session, const NetPacketAvatarHeader &tmpPacket)
{
	if (session.playerData.get())
	{
		NetPacketAvatarHeader::Data headerData;
		tmpPacket.GetData(headerData);

		if (headerData.avatarFileSize >= MIN_AVATAR_FILE_SIZE && headerData.avatarFileSize <= MAX_AVATAR_FILE_SIZE)
		{
			boost::shared_ptr<AvatarData> tmpAvatarData(new AvatarData);
			tmpAvatarData->fileData.reserve(headerData.avatarFileSize);
			tmpAvatarData->fileType = headerData.avatarFileType;
			tmpAvatarData->reportedSize = headerData.avatarFileSize;
			// Ignore request id for now.

			session.playerData->SetNetAvatarData(tmpAvatarData);

			// Session is now receiving an avatar.
			session.sessionData->SetState(SessionData::ReceivingAvatar);
		}
		else
			SessionError(session, ERR_NET_AVATAR_TOO_LARGE);
	}
}

void
ServerLobbyThread::HandleNetPacketUnknownAvatar(SessionWrapper session, const NetPacketUnknownAvatar &/*tmpPacket*/)
{
	if (session.playerData.get())
	{
		// Free memory (just in case).
		session.playerData->SetNetAvatarData(boost::shared_ptr<AvatarData>());
		session.playerData->SetAvatarMD5(MD5Buf());
		// Start session.
		EstablishSession(session);
	}
}

void
ServerLobbyThread::HandleNetPacketAvatarFile(SessionWrapper session, const NetPacketAvatarFile &tmpPacket)
{
	if (session.playerData.get())
	{
		NetPacketAvatarFile::Data data;
		tmpPacket.GetData(data);

		boost::shared_ptr<AvatarData> tmpAvatar = session.playerData->GetNetAvatarData();
		if (tmpAvatar.get() && tmpAvatar->fileData.size() + data.fileData.size() <= tmpAvatar->reportedSize)
		{
			std::copy(data.fileData.begin(), data.fileData.end(), back_inserter(tmpAvatar->fileData));
		}
	}
}

void
ServerLobbyThread::HandleNetPacketAvatarEnd(SessionWrapper session, const NetPacketAvatarEnd &/*tmpPacket*/)
{
	if (session.playerData.get())
	{
		boost::shared_ptr<AvatarData> tmpAvatar = session.playerData->GetNetAvatarData();
		MD5Buf avatarMD5 = session.playerData->GetAvatarMD5();
		if (!avatarMD5.IsZero() && tmpAvatar.get())
		{
			unsigned avatarSize = (unsigned)tmpAvatar->fileData.size();
			if (avatarSize == tmpAvatar->reportedSize)
			{
				if (!GetAvatarManager().StoreAvatarInCache(avatarMD5, tmpAvatar->fileType, &tmpAvatar->fileData[0], avatarSize, true))
				{
					session.playerData->SetAvatarMD5(MD5Buf());
					LOG_ERROR("Failed to store avatar in cache directory.");
				}

				// Free memory.
				session.playerData->SetNetAvatarData(boost::shared_ptr<AvatarData>());
				// Set avatar file name.
				string avatarFileName;
				if (GetAvatarManager().GetAvatarFileName(avatarMD5, avatarFileName))
					session.playerData->SetAvatarFile(avatarFileName);
				// Init finished - start session.
				EstablishSession(session);
				LOG_MSG("Client \"" << session.sessionData->GetClientAddr() << "\" uploaded avatar \""
					<< boost::filesystem::path(avatarFileName).file_string() << "\".");
			}
			else
				SessionError(session, ERR_NET_WRONG_AVATAR_SIZE);
		}
	}
}

void
ServerLobbyThread::HandleNetPacketRetrievePlayerInfo(SessionWrapper session, const NetPacketRetrievePlayerInfo &tmpPacket)
{
	NetPacketRetrievePlayerInfo::Data request;
	tmpPacket.GetData(request);

	// Find player in lobby or in a game.
	boost::shared_ptr<PlayerData> tmpPlayer = m_sessionManager.GetSessionByUniquePlayerId(request.playerId).playerData;
	if (!tmpPlayer.get())
		tmpPlayer = m_gameSessionManager.GetSessionByUniquePlayerId(request.playerId).playerData;
	if (!tmpPlayer.get())
	{
		boost::mutex::scoped_lock lock(m_computerPlayersMutex);
		PlayerDataMap::const_iterator pos = m_computerPlayers.find(request.playerId);
		if (pos != m_computerPlayers.end())
			tmpPlayer = pos->second;
	}

	if (tmpPlayer.get())
	{
		// Send player info to client.
		boost::shared_ptr<NetPacket> info(new NetPacketPlayerInfo);
		NetPacketPlayerInfo::Data infoData;
		infoData.playerId = tmpPlayer->GetUniqueId();
		infoData.playerInfo.ptype = tmpPlayer->GetType();
		infoData.playerInfo.playerName = tmpPlayer->GetName();
		infoData.playerInfo.hasAvatar = !tmpPlayer->GetAvatarMD5().IsZero();
		if (infoData.playerInfo.hasAvatar)
		{
			infoData.playerInfo.avatar = tmpPlayer->GetAvatarMD5();
			infoData.playerInfo.avatarType = AvatarManager::GetAvatarFileType(tmpPlayer->GetAvatarFile());
		}
		static_cast<NetPacketPlayerInfo *>(info.get())->SetData(infoData);
		session.sessionData->GetSender().Send(session.sessionData, info);
	}
	else
	{
		// Unknown player id - notify client.
		boost::shared_ptr<NetPacket> unknown(new NetPacketUnknownPlayerId);
		NetPacketUnknownPlayerId::Data unknownData;
		unknownData.playerId = request.playerId;
		static_cast<NetPacketUnknownPlayerId *>(unknown.get())->SetData(unknownData);
		session.sessionData->GetSender().Send(session.sessionData, unknown);
	}
}

void
ServerLobbyThread::HandleNetPacketRetrieveAvatar(SessionWrapper session, const NetPacketRetrieveAvatar &tmpPacket)
{
	bool avatarFound = false;
	NetPacketRetrieveAvatar::Data request;
	tmpPacket.GetData(request);

	string tmpFile;
	if (GetAvatarManager().GetAvatarFileName(request.avatar, tmpFile))
	{
		NetPacketList tmpPackets;
		if (GetAvatarManager().AvatarFileToNetPackets(tmpFile, request.requestId, tmpPackets) == 0)
		{
			avatarFound = true;
			session.sessionData->GetSender().Send(session.sessionData, tmpPackets);
		}
		else
			LOG_ERROR("Failed to read avatar file for network transmission.");
	}

	if (!avatarFound)
	{
		// Notify client we didn't find the avatar.
		boost::shared_ptr<NetPacket> unknown(new NetPacketUnknownAvatar);
		NetPacketUnknownAvatar::Data unknownData;
		unknownData.requestId = request.requestId;
		static_cast<NetPacketUnknownAvatar *>(unknown.get())->SetData(unknownData);
		session.sessionData->GetSender().Send(session.sessionData, unknown);
	}
}

void
ServerLobbyThread::HandleNetPacketCreateGame(SessionWrapper session, const NetPacketCreateGame &tmpPacket)
{
	LOG_VERBOSE("Creating new game, initiated by session #" << session.sessionData->GetId() << ".");

	// Create a new game.
	NetPacketCreateGame::Data createGameData;
	tmpPacket.GetData(createGameData);

	boost::shared_ptr<ServerGame> game(
		new ServerGame(
			*this,
			GetNextGameId(),
			createGameData.gameName,
			createGameData.password,
			createGameData.gameData,
			session.playerData->GetUniqueId(),
			GetGui(),
			m_playerConfig));

	MoveSessionToGame(*game, session);

	// Add game to list of games.
	InternalAddGame(game);
}

void
ServerLobbyThread::HandleNetPacketJoinGame(SessionWrapper session, const NetPacketJoinGame &tmpPacket)
{
	// Join an existing game.
	NetPacketJoinGame::Data joinGameData;
	tmpPacket.GetData(joinGameData);

	GameMap::iterator pos = m_gameMap.find(joinGameData.gameId);

	if (pos != m_gameMap.end())
	{
		ServerGame &game = *pos->second;
		if (game.CheckPassword(joinGameData.password))
		{
			MoveSessionToGame(game, session);
		}
		else
		{
			SendJoinGameFailed(session.sessionData, NTF_NET_JOIN_INVALID_PASSWORD);
		}
	}
	else
	{
		SessionError(session, ERR_NET_UNKNOWN_GAME);
	}
}

void
ServerLobbyThread::EstablishSession(SessionWrapper session)
{
	if (!session.playerData.get())
		throw ServerException(__FILE__, __LINE__, ERR_NET_INVALID_SESSION, 0);
	// Send ACK to client.
	boost::shared_ptr<NetPacket> initAck(new NetPacketInitAck);
	NetPacketInitAck::Data initAckData;
	initAckData.latestGameVersion = POKERTH_VERSION;
	initAckData.latestBetaRevision = POKERTH_BETA_REVISION;
	initAckData.sessionId = session.sessionData->GetId(); // TODO: currently unused.
	initAckData.playerId = session.playerData->GetUniqueId();
	static_cast<NetPacketInitAck *>(initAck.get())->SetData(initAckData);
	session.sessionData->GetSender().Send(session.sessionData, initAck);

	// Send the game list to the client.
	SendGameList(session.sessionData);

	// Session is now established.
	session.sessionData->SetState(SessionData::Established);

	{
		boost::mutex::scoped_lock lock(m_statMutex);
		++m_statData.totalPlayersEverLoggedIn;
		m_statDataChanged = true;
	}
	UpdateStatisticsNumberOfPlayers();
}

void
ServerLobbyThread::RequestPlayerAvatar(SessionWrapper session)
{
	if (!session.playerData.get())
		throw ServerException(__FILE__, __LINE__, ERR_NET_INVALID_SESSION, 0);
	// Accept no more new avatars from that client for a certain time.
	{
		boost::mutex::scoped_lock lock(m_timerAvatarClientAddressMapMutex);
		m_timerAvatarClientAddressMap[session.sessionData->GetClientAddr()] = boost::timers::portable::microsec_timer();
	}
	// Ask the client to send its avatar.
	boost::shared_ptr<NetPacket> retrieveAvatar(new NetPacketRetrieveAvatar);
	NetPacketRetrieveAvatar::Data retrieveAvatarData;
	retrieveAvatarData.requestId = session.playerData->GetUniqueId();
	retrieveAvatarData.avatar = session.playerData->GetAvatarMD5();
	static_cast<NetPacketRetrieveAvatar *>(retrieveAvatar.get())->SetData(retrieveAvatarData);
	session.sessionData->GetSender().Send(session.sessionData, retrieveAvatar);
}

void
ServerLobbyThread::TimerRemoveGame()
{
	// Synchronously remove games which have been closed.
	GameMap::iterator i = m_gameMap.begin();
	GameMap::iterator end = m_gameMap.end();
	while (i != end)
	{
		GameMap::iterator next = i;
		++next;
		boost::shared_ptr<ServerGame> tmpGame = i->second;
		if (!tmpGame->GetSessionManager().HasSessions())
			InternalRemoveGame(tmpGame); // This will delete the entry from the map.
		i = next;
	}
}

void
ServerLobbyThread::TimerRemovePlayer()
{
	boost::mutex::scoped_lock lock(m_removePlayerListMutex);

	if (!m_removePlayerList.empty())
	{
		RemovePlayerList::iterator i = m_removePlayerList.begin();
		RemovePlayerList::iterator end = m_removePlayerList.end();

		while (i != end)
		{
			InternalRemovePlayer(i->first, i->second);
			++i;
		}
		m_removePlayerList.clear();
	}
}

void
ServerLobbyThread::TimerUpdateClientAvatarLock()
{
	boost::mutex::scoped_lock lock(m_timerAvatarClientAddressMapMutex);

	TimerClientAddressMap::iterator i = m_timerAvatarClientAddressMap.begin();
	TimerClientAddressMap::iterator end = m_timerAvatarClientAddressMap.end();

	while (i != end)
	{
		TimerClientAddressMap::iterator next = i;
		++next;
		if (i->second.elapsed().total_seconds() > SERVER_INIT_AVATAR_CLIENT_LOCK_SEC)
			m_timerAvatarClientAddressMap.erase(i);
		i = next;
	}
}

void
ServerLobbyThread::TimerCheckSessionTimeouts()
{
	m_sessionManager.ForEach(boost::bind(&ServerLobbyThread::InternalCheckSessionTimeouts, boost::ref(*this), _1));
	m_gameSessionManager.ForEach(boost::bind(&ServerLobbyThread::InternalCheckSessionTimeouts, boost::ref(*this), _1));
}

void
ServerLobbyThread::TimerCleanupAvatarCache()
{
	// Only act if there are no sessions.
	if (!m_sessionManager.HasSessions() && !m_gameSessionManager.HasSessions())
	{
		LOG_VERBOSE("Cleaning up avatar cache.");

		m_avatarManager.RemoveOldAvatarCacheEntries();
	}
}

boost::shared_ptr<ServerGame>
ServerLobbyThread::InternalGetGameFromId(unsigned gameId)
{
	boost::shared_ptr<ServerGame> game;
	if (gameId)
	{
		GameMap::iterator pos = m_gameMap.find(gameId);

		if (pos != m_gameMap.end())
			game = pos->second;
	}
	return game;
}

void
ServerLobbyThread::InternalAddGame(boost::shared_ptr<ServerGame> game)
{
	// Add game to list.
	m_gameMap.insert(GameMap::value_type(game->GetId(), game));
	// Notify all players.
	m_sessionManager.SendLobbyMsgToAllSessions(CreateNetPacketGameListNew(*game), SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(CreateNetPacketGameListNew(*game), SessionData::Game);

	{
		boost::mutex::scoped_lock lock(m_statMutex);
		++m_statData.totalGamesEverCreated;
		++m_statData.numberOfGamesOpen;
		unsigned numGames = static_cast<unsigned>(m_gameMap.size());
		if (numGames > m_statData.maxGamesOpen)
			m_statData.maxGamesOpen = numGames;
		m_statDataChanged = true;
	}
}

void
ServerLobbyThread::InternalRemoveGame(boost::shared_ptr<ServerGame> game)
{
	{
		boost::mutex::scoped_lock lock(m_statMutex);
		if (m_statData.numberOfGamesOpen)
		{
			--m_statData.numberOfGamesOpen;
			m_statDataChanged = true;
		}
	}
	// Remove game from list.
	m_gameMap.erase(game->GetId());
	// Remove all sessions left in the game.
	game->ResetComputerPlayerList();
	game->RemoveAllSessions();
	// Notify all players.
	boost::shared_ptr<NetPacket> packet = CreateNetPacketGameListUpdate(game->GetId(), GAME_MODE_CLOSED);
	m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
	m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
}

void
ServerLobbyThread::InternalRemovePlayer(unsigned playerId, unsigned errorCode)
{
	SessionWrapper session = m_sessionManager.GetSessionByUniquePlayerId(playerId);
	if (session.sessionData.get())
		SessionError(session, errorCode);
	else
	{
		// Scan games for the player.
		GameMap::iterator i = m_gameMap.begin();
		GameMap::iterator end = m_gameMap.end();

		while (i != end)
		{
			boost::shared_ptr<ServerGame> tmpGame = i->second;
			if (tmpGame->GetPlayerDataByUniqueId(playerId).get())
			{
				tmpGame->RemovePlayer(playerId, errorCode);
				break;
			}
			++i;
		}
	}
}

void
ServerLobbyThread::InternalResubscribeMsg(SessionWrapper session)
{
	if (!session.sessionData->WantsLobbyMsg())
	{
		session.sessionData->SetWantsLobbyMsg();
		SendGameList(session.sessionData);
		// Send new statistics information.
		boost::shared_ptr<NetPacket> packet(new NetPacketStatisticsChanged);
		NetPacketStatisticsChanged::Data statData;
		statData.stats.numberOfPlayersOnServer = m_sessionManager.GetRawSessionCount() + m_gameSessionManager.GetRawSessionCount();
		try {
			static_cast<NetPacketStatisticsChanged *>(packet.get())->SetData(statData);

			session.sessionData->GetSender().Send(session.sessionData, packet);
		} catch (const NetException &)
		{
			// Ignore errors for now.
		}
	}
}

void
ServerLobbyThread::HandleReAddedSession(SessionWrapper session)
{
	// Remove session from game session list.
	m_gameSessionManager.RemoveSession(session.sessionData->GetId());

	if (m_sessionManager.GetRawSessionCount() <= SERVER_MAX_NUM_SESSIONS)
	{
		// Set state (back) to established.
		session.sessionData->SetState(SessionData::Established);
		session.sessionData->SetGameId(0);
		// Add session to lobby list.
		m_sessionManager.AddSession(session);
	}
	else
	{
		// Gracefully close this session.
		SessionError(session, ERR_NET_SERVER_FULL);
	}
}

void
ServerLobbyThread::InternalCheckSessionTimeouts(SessionWrapper session)
{
	bool closeSession = false;
	if (session.sessionData.get() && session.playerData.get())
	{
		if (session.sessionData->GetState() == SessionData::Init && session.sessionData->GetAutoDisconnectTimerElapsedSec() >= SERVER_INIT_SESSION_TIMEOUT_SEC)
		{
			LOG_VERBOSE("Session init timeout, removing session #" << session.sessionData->GetId() << ".");
			closeSession = true;
		}
		else if (session.sessionData->GetActivityTimerElapsedSec() >= SERVER_SESSION_ACTIVITY_TIMEOUT_SEC - SERVER_TIMEOUT_WARNING_REMAINING_SEC
				&& !session.sessionData->HasActivityNoticeBeenSent())
		{
			session.sessionData->MarkActivityNotice();
			boost::shared_ptr<NetPacket> packet(new NetPacketTimeoutWarning);
			NetPacketTimeoutWarning::Data warningData;
			warningData.timeoutReason = NETWORK_TIMEOUT_GENERIC;
			warningData.remainingSeconds = SERVER_TIMEOUT_WARNING_REMAINING_SEC;
			static_cast<NetPacketTimeoutWarning *>(packet.get())->SetData(warningData);
			session.sessionData->GetSender().Send(session.sessionData, packet);
		}
		else if (session.sessionData->GetActivityTimerElapsedSec() >= SERVER_SESSION_ACTIVITY_TIMEOUT_SEC)
		{
			LOG_VERBOSE("Activity timeout, removing session #" << session.sessionData->GetId() << ".");
			closeSession = true;
		}
		else if (session.sessionData->GetAutoDisconnectTimerElapsedSec() >= SERVER_SESSION_FORCED_TIMEOUT_SEC)
		{
			LOG_VERBOSE("Auto disconnect timeout, removing session #" << session.sessionData->GetId() << ".");
			closeSession = true;
		}
	}
	if (closeSession)
	{
		RemovePlayer(session.playerData->GetUniqueId(), ERR_NET_SESSION_TIMED_OUT);
	}
}

void
ServerLobbyThread::SessionError(SessionWrapper session, int errorCode)
{
	if (session.sessionData.get())
	{
		SendError(session.sessionData, errorCode);
		CloseSession(session);
	}
}

void
ServerLobbyThread::SendError(boost::shared_ptr<SessionData> s, int errorCode)
{
	LOG_VERBOSE("Sending error code " << errorCode << " to session #" << s->GetId() << ".");
	boost::shared_ptr<NetPacket> packet(new NetPacketError);
	NetPacketError::Data errorData;
	errorData.errorCode = errorCode;
	static_cast<NetPacketError *>(packet.get())->SetData(errorData);
	s->GetSender().Send(s, packet);
}

void
ServerLobbyThread::SendJoinGameFailed(boost::shared_ptr<SessionData> s, int reason)
{
	boost::shared_ptr<NetPacket> packet(new NetPacketJoinGameFailed);
	NetPacketJoinGameFailed::Data failedData;
	failedData.failureCode = reason;
	static_cast<NetPacketJoinGameFailed *>(packet.get())->SetData(failedData);
	s->GetSender().Send(s, packet);
}

void
ServerLobbyThread::SendGameList(boost::shared_ptr<SessionData> s)
{
	GameMap::const_iterator game_i = m_gameMap.begin();
	GameMap::const_iterator game_end = m_gameMap.end();
	while (game_i != game_end)
	{
		s->GetSender().Send(s, CreateNetPacketGameListNew(*game_i->second));
		++game_i;
	}
}

void
ServerLobbyThread::UpdateStatisticsNumberOfPlayers()
{
	ServerStats stats;
	unsigned curNumberOfPlayersOnServer = m_sessionManager.GetRawSessionCount() + m_gameSessionManager.GetRawSessionCount();
	{
		boost::mutex::scoped_lock lock(m_statMutex);
		if (curNumberOfPlayersOnServer != m_statData.numberOfPlayersOnServer)
		{
			m_statData.numberOfPlayersOnServer = stats.numberOfPlayersOnServer = curNumberOfPlayersOnServer;
			if (curNumberOfPlayersOnServer > m_statData.maxPlayersLoggedIn)
				m_statData.maxPlayersLoggedIn = curNumberOfPlayersOnServer;
			m_statDataChanged = true;
		}
	}
	// Do not send other stats than number of players for now.
	BroadcastStatisticsUpdate(stats);
}

void
ServerLobbyThread::BroadcastStatisticsUpdate(const ServerStats &stats)
{
	if (stats.numberOfPlayersOnServer)
	{
		boost::shared_ptr<NetPacket> packet(new NetPacketStatisticsChanged);
		NetPacketStatisticsChanged::Data statData;
		statData.stats = stats;
		try {
			static_cast<NetPacketStatisticsChanged *>(packet.get())->SetData(statData);

			m_sessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Established);
			m_gameSessionManager.SendLobbyMsgToAllSessions(packet, SessionData::Game);
		} catch (const NetException &)
		{
			// Ignore errors for now.
			//LOG_ERROR("ServerLobbyThread::BroadcastStatisticsUpdate: " << e.what());
		}
	}
}

void
ServerLobbyThread::ReadStatisticsFile()
{
	ifstream i(m_statisticsFileName.c_str(), ios_base::in);

	if (!i.fail() && !i.eof())
	{
		boost::mutex::scoped_lock lock(m_statMutex);
		do
		{
			string statisticsType;
			unsigned statisticsValue;
			i >> statisticsType;
			i >> statisticsValue;
			if (statisticsType == SERVER_STATISTICS_STR_TOTAL_PLAYERS)
				m_statData.totalPlayersEverLoggedIn = statisticsValue;
			else if (statisticsType == SERVER_STATISTICS_STR_TOTAL_GAMES)
				m_statData.totalGamesEverCreated = statisticsValue;
			else if (statisticsType == SERVER_STATISTICS_STR_MAX_PLAYERS)
				m_statData.maxPlayersLoggedIn = statisticsValue;
			else if (statisticsType == SERVER_STATISTICS_STR_MAX_GAMES)
				m_statData.maxGamesOpen = statisticsValue;
			// other statistics are non-persistant and not read.
		} while (!i.fail() && !i.eof());
		m_statDataChanged = false;
	}
}

void
ServerLobbyThread::TimerSaveStatisticsFile()
{
	LOG_VERBOSE("Saving statistics.");
	boost::mutex::scoped_lock lock(m_statMutex);
	if (m_statDataChanged)
	{
		ofstream o(m_statisticsFileName.c_str(), ios_base::out | ios_base::trunc);
		if (!o.fail())
		{
			o << SERVER_STATISTICS_STR_TOTAL_PLAYERS " " << m_statData.totalPlayersEverLoggedIn << endl;
			o << SERVER_STATISTICS_STR_TOTAL_GAMES " " << m_statData.totalGamesEverCreated << endl;
			o << SERVER_STATISTICS_STR_MAX_PLAYERS " " << m_statData.maxPlayersLoggedIn << endl;
			o << SERVER_STATISTICS_STR_MAX_GAMES " " << m_statData.maxGamesOpen << endl;
			o << SERVER_STATISTICS_STR_CUR_PLAYERS " " << m_statData.numberOfPlayersOnServer << endl;
			o << SERVER_STATISTICS_STR_CUR_GAMES " " << m_statData.numberOfGamesOpen << endl;
			m_statDataChanged = false;
		}
	}
}

ServerCallback &
ServerLobbyThread::GetCallback()
{
	return m_gui;
}

ReceiverHelper &
ServerLobbyThread::GetReceiver()
{
	assert(m_receiver.get());
	return *m_receiver;
}

bool
ServerLobbyThread::CheckPassword(const string &password) const
{
	return (password == m_password);
}

ServerSenderCallback &
ServerLobbyThread::GetSenderCallback()
{
	assert(m_senderCallback.get());
	return *m_senderCallback;
}

GuiInterface &
ServerLobbyThread::GetGui()
{
	return m_gui;
}

bool
ServerLobbyThread::IsPlayerConnected(const string &name) const
{
	bool retVal = false;

	retVal = m_sessionManager.IsPlayerConnected(name);

	if (!retVal)
		retVal = m_gameSessionManager.IsPlayerConnected(name);

	return retVal;
}

bool
ServerLobbyThread::IsPlayerBanned(const std::string &name) const
{
	bool retVal = false;
	boost::mutex::scoped_lock lock(m_banMutex);
	RegexMap::const_iterator i = m_banPlayerNameMap.begin();
	RegexMap::const_iterator end = m_banPlayerNameMap.end();
	while (i != end)
	{
		if (regex_match(name, (*i).second))
		{
			retVal = true;
			break;
		}
		++i;
	}

	return retVal;
}

bool
ServerLobbyThread::IsIPAddressBanned(const std::string &ipAddress) const
{
	bool retVal = false;
	boost::mutex::scoped_lock lock(m_banMutex);
	IPAddressMap::const_iterator i = m_banIPAddressMap.begin();
	IPAddressMap::const_iterator end = m_banIPAddressMap.end();
	while (i != end)
	{
		if (ipAddress == (*i).second)
		{
			retVal = true;
			break;
		}
		++i;
	}

	return retVal;
}

boost::shared_ptr<NetPacket>
ServerLobbyThread::CreateNetPacketGameListNew(const ServerGame &game)
{
	boost::shared_ptr<NetPacket> packet(new NetPacketGameListNew);
	NetPacketGameListNew::Data packetData;
	packetData.gameId = game.GetId();
	packetData.gameInfo.adminPlayerId = game.GetAdminPlayerId();
	packetData.gameInfo.mode = game.IsRunning() ? GAME_MODE_STARTED : GAME_MODE_CREATED;
	packetData.gameInfo.name = game.GetName();
	packetData.gameInfo.data = game.GetGameData();
	packetData.gameInfo.players = game.GetPlayerIdList();
	packetData.gameInfo.isPasswordProtected = game.IsPasswordProtected();
	static_cast<NetPacketGameListNew *>(packet.get())->SetData(packetData);
	return packet;
}

boost::shared_ptr<NetPacket>
ServerLobbyThread::CreateNetPacketGameListUpdate(unsigned gameId, GameMode mode)
{
	boost::shared_ptr<NetPacket> packet(new NetPacketGameListUpdate);
	NetPacketGameListUpdate::Data packetData;
	packetData.gameId = gameId;
	packetData.gameMode = mode;
	static_cast<NetPacketGameListUpdate *>(packet.get())->SetData(packetData);
	return packet;
}

