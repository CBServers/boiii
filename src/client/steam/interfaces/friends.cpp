#include <std_include.hpp>
#include "../steam.hpp"

#include <utils/nt.hpp>
#include <utils/string.hpp>

#include "component/name.hpp"
#include "component/chat.hpp"
#include "component/friends.hpp"

namespace steam
{
	namespace
	{
		// 24-byte FriendGameInfo_t (matches Steamworks / boiii-free)
		struct friend_game_info
		{
			game_id game;
			unsigned int game_ip;
			unsigned short game_port;
			unsigned short query_port;
			steam_id lobby;
		};

		// fake lobby id so an "in game" friend has something joinable (mirrors CreateLobby)
		steam_id make_lobby_id(const unsigned int account_id)
		{
			steam_id id{};
			id.raw.account_id = account_id;
			id.raw.account_instance = 0x40000;
			id.raw.account_type = 8;
			id.raw.universe = 1;
			return id;
		}

		bool find_cb_friend(const steam_id id, ::friends::friend_record& out)
		{
			return ::friends::find_friend(id.bits, out);
		}

		// temporary crash diagnostics, remove once the launcher feed is verified
		void debug_log(const std::string& line)
		{
			static std::mutex mutex;
			std::lock_guard _(mutex);
			static std::ofstream file("boiii_friends_debug.log", std::ios::app);
			file << line << std::endl;
		}
	}

	const char* friends::GetPersonaName()
	{
		return name::get_player_name();
	}

	unsigned long long friends::SetPersonaName(const char* pchPersonaName)
	{
		return 0;
	}

	int friends::GetPersonaState()
	{
		return 1;
	}

	int friends::GetFriendCount(int eFriendFlags)
	{
		const auto count = static_cast<int>(::friends::get_friends().size());
		debug_log(::utils::string::va("GetFriendCount(0x%X) -> %d", eFriendFlags, count));
		return count;
	}

	steam_id friends::GetFriendByIndex(int iFriend, int iFriendFlags)
	{
		debug_log(::utils::string::va("GetFriendByIndex(%d, 0x%X)", iFriend, iFriendFlags));
		const auto list = ::friends::get_friends();
		if (iFriend < 0 || iFriend >= static_cast<int>(list.size()))
		{
			return steam_id();
		}

		steam_id id{};
		id.bits = list[iFriend].steam_id_bits;
		return id;
	}

	int friends::GetFriendRelationship(steam_id steamIDFriend)
	{
		::friends::friend_record record{};
		return find_cb_friend(steamIDFriend, record) ? 3 : 0;
	}

	int friends::GetFriendPersonaState(steam_id steamIDFriend)
	{
		::friends::friend_record record{};
		return find_cb_friend(steamIDFriend, record) ? record.persona_state : 0;
	}

	const char* friends::GetFriendPersonaName(steam_id steamIDFriend)
	{
		::friends::friend_record record{};
		if (find_cb_friend(steamIDFriend, record))
		{
			static thread_local std::string name_buffer;
			name_buffer = record.name;
			return name_buffer.data();
		}

		return chat::get_client_name(steamIDFriend.bits);
	}

	bool friends::GetFriendGamePlayed(steam_id steamIDFriend, void* pFriendGameInfo)
	{
		::friends::friend_record record{};
		if (!find_cb_friend(steamIDFriend, record) || !record.in_game)
		{
			return false;
		}

		if (pFriendGameInfo)
		{
			auto* info = static_cast<friend_game_info*>(pFriendGameInfo);
			info->game.raw.app_id = 311210; // Black Ops III Steam AppID
			info->game_ip = 0;
			info->game_port = 0;
			info->query_port = 0;
			info->lobby = make_lobby_id(steamIDFriend.raw.account_id);
		}

		debug_log(::utils::string::va("GetFriendGamePlayed(%llX) -> true (in game)", steamIDFriend.bits));
		return true;
	}

	const char* friends::GetFriendPersonaNameHistory(steam_id steamIDFriend, int iPersonaName)
	{
		return "";
	}

	int friends::GetFriendSteamLevel(steam_id steamIDFriend)
	{
		return 0;
	}

	const char* friends::GetPlayerNickname(steam_id steamIDPlayer)
	{
		return nullptr;
	}

	int friends::GetFriendsGroupCount()
	{
		return 0;
	}

	short friends::GetFriendsGroupIDByIndex(int iFG)
	{
		return -1;
	}

	const char* friends::GetFriendsGroupName(short friendsGroupID)
	{
		return nullptr;
	}

	int friends::GetFriendsGroupMembersCount(short friendsGroupID)
	{
		return 0;
	}

	void friends::GetFriendsGroupMembersList(short friendsGroupID, steam_id* pOutSteamIDMembers, int nMembersCount)
	{
	}

	bool friends::HasFriend(steam_id steamIDFriend, int eFriendFlags)
	{
		::friends::friend_record record{};
		return find_cb_friend(steamIDFriend, record);
	}

	int friends::GetClanCount()
	{
		return 0;
	}

	steam_id friends::GetClanByIndex(int iClan)
	{
		return steam_id();
	}

	const char* friends::GetClanName(steam_id steamIDClan)
	{
		return "3arc";
	}

	const char* friends::GetClanTag(steam_id steamIDClan)
	{
		return this->GetClanName(steamIDClan);
	}

	bool friends::GetClanActivityCounts(steam_id steamID, int* pnOnline, int* pnInGame, int* pnChatting)
	{
		return false;
	}

	unsigned long long friends::DownloadClanActivityCounts(steam_id groupIDs[], int nIds)
	{
		return 0;
	}

	int friends::GetFriendCountFromSource(steam_id steamIDSource)
	{
		return 0;
	}

	steam_id friends::GetFriendFromSourceByIndex(steam_id steamIDSource, int iFriend)
	{
		return steam_id();
	}

	bool friends::IsUserInSource(steam_id steamIDUser, steam_id steamIDSource)
	{
		return false;
	}

	void friends::SetInGameVoiceSpeaking(steam_id steamIDUser, bool bSpeaking)
	{
	}

	void friends::ActivateGameOverlay(const char* pchDialog)
	{
	}

	void friends::ActivateGameOverlayToUser(const char* pchDialog, steam_id steamID)
	{
	}

	void friends::ActivateGameOverlayToWebPage(const char* pchURL)
	{
	}

	void friends::ActivateGameOverlayToStore(unsigned int nAppID, unsigned int eFlag)
	{
	}

	void friends::SetPlayedWith(steam_id steamIDUserPlayedWith)
	{
	}

	void friends::ActivateGameOverlayInviteDialog(steam_id steamIDLobby)
	{
	}

	int friends::GetSmallFriendAvatar(steam_id steamIDFriend)
	{
		debug_log(::utils::string::va("GetSmallFriendAvatar(%llX)", steamIDFriend.bits));
		return 0;
	}

	int friends::GetMediumFriendAvatar(steam_id steamIDFriend)
	{
		debug_log(::utils::string::va("GetMediumFriendAvatar(%llX)", steamIDFriend.bits));
		return 0;
	}

	int friends::GetLargeFriendAvatar(steam_id steamIDFriend)
	{
		debug_log(::utils::string::va("GetLargeFriendAvatar(%llX)", steamIDFriend.bits));
		return 0;
	}

	bool friends::RequestUserInformation(steam_id steamIDUser, bool bRequireNameOnly)
	{
		debug_log(::utils::string::va("RequestUserInformation(%llX, %d)", steamIDUser.bits, bRequireNameOnly));
		return false;
	}

	unsigned long long friends::RequestClanOfficerList(steam_id steamIDClan)
	{
		return 0;
	}

	steam_id friends::GetClanOwner(steam_id steamIDClan)
	{
		return steam_id();
	}

	int friends::GetClanOfficerCount(steam_id steamIDClan)
	{
		return 0;
	}

	steam_id friends::GetClanOfficerByIndex(steam_id steamIDClan, int iOfficer)
	{
		return steam_id();
	}

	int friends::GetUserRestrictions()
	{
		return 0;
	}

	bool friends::SetRichPresence(const char* pchKey, const char* pchValue)
	{
		return true;
	}

	void friends::ClearRichPresence()
	{
	}

	const char* friends::GetFriendRichPresence(steam_id steamIDFriend, const char* pchKey)
	{
		::friends::friend_record record{};
		if (!find_cb_friend(steamIDFriend, record) || !record.in_game || !pchKey)
		{
			return "";
		}

		// The native presence parser strtol's these keys into the friend record, and
		// Engine.GetPlayerInfo() surfaces them as info.activity/mapid/gametype, which
		// GetPresenceActivityString turns into "Playing <gametype> on <map>".
		const std::string_view key = pchKey;
		const auto map_id = ::friends::lookup_map_id(record.map);
		const auto gametype_id = ::friends::lookup_gametype_id(record.gametype);
		const bool zm = record.mode == "zm";
		// mp+map+gametype => 33 (MP_PLAYING_GMODE_ON_MAP), zm+map => 49 (ZM_PLAYING_MAP_ON_ROUND),
		// unresolvable map/gametype => 34 (MP_IN_LOBBY) as a generic in-title fallback
		const bool resolved = map_id >= 0 && (zm || gametype_id >= 0);
		const auto activity = !resolved ? 34 : (zm ? 49 : 33);

		static thread_local std::string value_buffer;
		value_buffer.clear();

		if (key == "version" || key == "state") value_buffer = "1";
		else if (key == "tActivity") value_buffer = std::to_string(activity);
		else if (key == "tJoinable") value_buffer = record.joinable ? "1" : "0";
		else if (key == "tMapId" && resolved) value_buffer = std::to_string(map_id);
		else if (key == "tGametype" && resolved && !zm) value_buffer = std::to_string(gametype_id);

		return value_buffer.data();
	}

	int friends::GetFriendRichPresenceKeyCount(steam_id steamIDFriend)
	{
		debug_log(::utils::string::va("GetFriendRichPresenceKeyCount(%llX)", steamIDFriend.bits));
		return 0;
	}

	const char* friends::GetFriendRichPresenceKeyByIndex(steam_id steamIDFriend, int iKey)
	{
		debug_log(::utils::string::va("GetFriendRichPresenceKeyByIndex(%llX, %d)", steamIDFriend.bits, iKey));
		return "a";
	}

	void friends::RequestFriendRichPresence(steam_id steamIDFriend)
	{
		debug_log(::utils::string::va("RequestFriendRichPresence(%llX)", steamIDFriend.bits));
	}

	bool friends::InviteUserToGame(steam_id steamIDFriend, const char* pchConnectString)
	{
		debug_log(::utils::string::va("*** InviteUserToGame(%llX, %s)", steamIDFriend.bits,
		                              pchConnectString ? pchConnectString : "null"));
		return ::friends::request_invite(steamIDFriend.bits);
	}

	int friends::GetCoplayFriendCount()
	{
		return 0;
	}

	steam_id friends::GetCoplayFriend(int iCoplayFriend)
	{
		return steam_id();
	}

	int friends::GetFriendCoplayTime(steam_id steamIDFriend)
	{
		return 0;
	}

	unsigned int friends::GetFriendCoplayGame(steam_id steamIDFriend)
	{
		return 0;
	}

	unsigned long long friends::JoinClanChatRoom(steam_id steamIDClan)
	{
		return 0;
	}

	bool friends::LeaveClanChatRoom(steam_id steamIDClan)
	{
		return false;
	}

	int friends::GetClanChatMemberCount(steam_id steamIDClan)
	{
		return 0;
	}

	steam_id friends::GetChatMemberByIndex(steam_id steamIDClan, int iUser)
	{
		return steam_id();
	}

	bool friends::SendClanChatMessage(steam_id steamIDClanChat, const char* pchText)
	{
		return false;
	}

	int friends::GetClanChatMessage(steam_id steamIDClanChat, int iMessage, void* prgchText, int cchTextMax,
	                                unsigned int* peChatEntryType, steam_id* pSteamIDChatter)
	{
		return 0;
	}

	bool friends::IsClanChatAdmin(steam_id steamIDClanChat, steam_id steamIDUser)
	{
		return false;
	}

	bool friends::IsClanChatWindowOpenInSteam(steam_id steamIDClanChat)
	{
		return false;
	}

	bool friends::OpenClanChatWindowInSteam(steam_id steamIDClanChat)
	{
		return false;
	}

	bool friends::CloseClanChatWindowInSteam(steam_id steamIDClanChat)
	{
		return false;
	}

	bool friends::SetListenForFriendsMessages(bool bInterceptEnabled)
	{
		return false;
	}

	bool friends::ReplyToFriendMessage(steam_id steamIDFriend, const char* pchMsgToSend)
	{
		return false;
	}

	int friends::GetFriendMessage(steam_id steamIDFriend, int iMessageID, void* pvData, int cubData,
	                              unsigned int* peChatEntryType)
	{
		return 0;
	}

	unsigned long long friends::GetFollowerCount(steam_id steamID)
	{
		return 0;
	}

	unsigned long long friends::IsFollowing(steam_id steamID)
	{
		return 0;
	}

	unsigned long long friends::EnumerateFollowingList(unsigned int unStartIndex)
	{
		return 0;
	}

}
