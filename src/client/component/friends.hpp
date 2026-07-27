#pragma once

#include <string>
#include <vector>

namespace friends
{
	// One CB-Launcher friend as consumed by the ISteamFriends stubs. steam_id_bits is a
	// synthetic, session-sticky id derived from the discord id (universe/type/instance 1).
	struct friend_record
	{
		unsigned long long steam_id_bits{0};
		std::string discord_id;
		std::string name;
		int persona_state{0}; // steam persona: 0 offline, 1 online, 2 busy, 3 away
		bool in_game{false};  // in a boiii match (launcher saw a party with game id "boiii")
		bool joinable{false};
		bool same_match{false}; // in the match we're already in; never joinable
		std::string mode;     // "mp" / "zm"
		std::string map;      // raw map name, e.g. "mp_nuketown_x"
		std::string gametype; // raw gametype name, e.g. "tdm"
	};

	// Parsed entry of the launcher's "friends" IPC snapshot, before steam id assignment.
	struct snapshot_entry
	{
		std::string discord_id;
		std::string name;
		std::string status; // "online" / "idle" / "dnd" / "offline"
		bool in_launcher{false};
		bool has_game{false};
		std::string game_id;
		std::string mode;
		std::string map;
		std::string gametype;
		bool joinable{false};
		bool same_match{false};
	};

	// Called from the IPC io thread; swaps the store atomically.
	void apply_snapshot(const std::vector<snapshot_entry>& entries);

	// Bumped on every apply_snapshot; LUI polls it to live-refresh the Social list.
	int snapshot_version();

	std::vector<friend_record> get_friends();
	bool find_friend(unsigned long long steam_id_bits, friend_record& out);
	bool is_joinable(unsigned long long steam_id_bits);

	// Raw-name -> native id tables, registered from LUI at UI load (game.registerFriend*).
	void register_map_id(const std::string& mapname, int unique_id);
	void register_gametype_id(const std::string& gametype, int id);
	int lookup_map_id(const std::string& mapname);      // -1 when unknown
	int lookup_gametype_id(const std::string& gametype); // -1 when unknown

	// Ask the launcher to join / invite a friend (sends over the IPC pipe).
	bool request_join(unsigned long long steam_id_bits);
	bool request_invite(unsigned long long steam_id_bits);
}
