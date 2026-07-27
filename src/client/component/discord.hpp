#pragma once

#include <optional>
#include <string>

namespace discord
{
	// Semantic game state shared by native RPC and launcher IPC; strings are color-stripped and truncated.
	struct presence_state
	{
		bool in_game{false};
		std::string mapname;     // raw map key, empty in menu
		std::string map_display; // friendly map name
		std::string gametype;    // friendly gametype name
		std::string gametype_raw; // raw gametype key, e.g. "tdm"
		std::string mode;        // short key: "mp" / "zm" / "sp"
		std::string server_name; // public dedicated server only, else empty
		int players{0};
		int max_players{0};
		bool openable{false};    // hosting a private match not yet open to friends
		std::string match_id;    // identity of the current match, identical for host and joiners
	};

	presence_state get_presence_state();

	// How a friend joins us, in the launcher's unified-secret terms (Feature 3).
	struct join_transport
	{
		bool is_nat{false};
		std::string ip;             // direct: server address
		int port{0};
		std::string token;          // nat: host punch token
		std::string rendezvous_host;
		int rendezvous_port{0};
		std::string fallback_ip;    // nat: host's reachable endpoint (port-forward/VPN/public)
		int fallback_port{0};
	};

	// The current joinable transport, or empty when not joinable (menu / private / unreachable).
	std::optional<join_transport> get_join_transport();

	// Queue a structured join to run once the game is ready (main menu up / in a match). Use this for
	// invite-driven joins: a cold-launched invite arrives mid-load and would crash if routed immediately.
	void queue_join(const std::string& token, const std::string& address);

	// Feature 4: launcher ownership signal from the IPC pipe. While the launcher owns presence the
	// native RPC goes silent (cleared once, connection kept alive); it resumes on release/pipe-drop.
	void set_launcher_presence_owner(bool launcher_owns);
}
