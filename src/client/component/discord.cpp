#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/utils.hpp"

#include "discord.hpp"
#include "getinfo.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include <discord_register.h>
#include <discord_rpc.h>

#include <utils/cryptography.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

#include <atomic>
#include <utility>

namespace discord
{
	namespace
	{
		constexpr auto* DISCORD_APP_ID = "1047539933922988112";
		constexpr auto* JOIN_SECRET_PREFIX = "boiii:1:";
		constexpr auto PRESENCE_HEARTBEAT = 60s;

		struct presence_snapshot
		{
			std::string details;
			std::string state;
			int64_t start_timestamp = 0;
			std::string large_image_key;
			std::string large_image_text;
			std::string small_image_key;
			std::string small_image_text;
			std::string party_id;
			int party_size = 0;
			int party_max = 0;
			int party_privacy = 0;
			std::string join_secret;

			bool operator==(const presence_snapshot&) const = default;
		};

		// Invite-driven joins (Feature 3) wait here until the game is ready to act on a connect.
		std::mutex pending_route_mutex;
		std::optional<std::pair<std::string, std::string>> pending_route; // (token, address)

		int64_t start_time = 0;
		int64_t match_time = 0;
		bool was_in_game = false;

		std::optional<presence_snapshot> last_presence;
		std::chrono::steady_clock::time_point last_presence_update{};

		// Feature 4 ownership handoff. wire_* is set from the IPC IO thread; the rest is main-thread only.
		constexpr auto OWNERSHIP_RELEASE_GRACE = 5s;
		std::atomic_bool wire_launcher_owns{false};
		bool effective_launcher_owns = false; // native RPC stays silent while true
		bool release_pending = false;
		std::chrono::steady_clock::time_point release_deadline{};

		int64_t unix_time()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
		}

		std::string strip_colors(const std::string& value)
		{
			std::string stripped;
			stripped.resize(value.size() + 1);
			utils::string::strip(value.data(), stripped.data(), stripped.size());
			stripped.resize(strlen(stripped.data()));
			return stripped;
		}

		std::string get_join_address()
		{
			if (!game::Com_IsInGame())
			{
				return {};
			}

			// Host: our reachable endpoint, paired with a real token to hole-punch.
			if (auto endpoint = nat::get_host_endpoint(); !endpoint.empty())
			{
				return endpoint;
			}

			// Client on a public dedi: advertise it (token "-"); the name check rejects stale/private connection state.
			if (!party::get_public_server_name().empty())
			{
				return network::address_to_string(party::get_connected_server());
			}

			return {};
		}

		std::string make_join_secret(const std::string& address)
		{
			if (address.empty())
			{
				return {};
			}

			// "boiii:1:<token>:<ip>:<port>"; token "-" means direct-only (no punch).
			const auto token = nat::current_token();
			const auto token_field = token.empty() ? std::string("-") : token;

			const auto secret = std::string(JOIN_SECRET_PREFIX) + token_field + ":" + address;
			if (secret.size() >= 128)
			{
				return {};
			}

			return secret;
		}

		bool parse_join_secret(const std::string& secret, std::string* token, std::string* address)
		{
			if (!utils::string::starts_with(secret, JOIN_SECRET_PREFIX))
			{
				return false;
			}

			// "<token>:<ip>:<port>"
			const auto raw = secret.substr(strlen(JOIN_SECRET_PREFIX));
			const auto sep = raw.find(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			*token = raw.substr(0, sep);

			const auto raw_address = raw.substr(sep + 1);
			const auto parsed = network::address_from_string(raw_address);
			if (!network::is_connectable_address(parsed))
			{
				return false;
			}

			*address = network::address_to_string(parsed);
			return true;
		}

		// Route a structured join: token "-"/empty => direct connect, else NAT punch. Main pipeline only.
		void route_join(const std::string& token, const std::string& address)
		{
			if (token.empty() || token == "-")
			{
				// party::connect directly; skips a command-buffer round trip.
				const auto target = network::address_from_string(address);
				if (network::is_connectable_address(target))
				{
					party::connect(target);
				}
				else
				{
					printf("Discord: invalid join address\n");
				}
				return;
			}

			// Hole-punch toward the host; falls back to `address` (port-forward/VPN).
			nat::begin_join(token, address);
		}

		// Connectivity flags reported once the player has clicked Play and online services are fully up.
		// Live connectivity alone goes true at the title screen (network reachable) but the lobby
		// subsystem isn't ready yet — routing a join there crashes. This exact value means lobby-ready.
		constexpr int ONLINE_READY_FLAGS = 536684543;

		// True once the game can actually act on a connect: online services report the lobby-ready flag.
		// A cold-launched invite otherwise routes into the lobby subsystem before sign-in and crashes.
		// The user must click "Play" first to bring online services up (auto-clicking proved unreliable).
		bool join_ready()
		{
			int connectivity = 0;
			const bool live = game::Live_GetConnectivityInformation(0, &connectivity, false);
			return live && connectivity == ONLINE_READY_FLAGS;
		}

		// Drains an invite-driven join, but only once join_ready(). A cold-launched invite reaches us
		// mid-load (the IPC pipe says hello during component init); routing too early crashes.
		void process_pending_route()
		{
			if (!join_ready())
			{
				return; // user hasn't clicked Play yet / online services still coming up; keep waiting
			}

			std::optional<std::pair<std::string, std::string>> route;
			{
				std::lock_guard lock(pending_route_mutex);
				route = std::move(pending_route);
				pending_route.reset();
			}

			if (route)
			{
				route_join(route->first, route->second);
			}
		}

		// Splits "ip:port" into its parts.
		bool split_address(const std::string& address, std::string& ip, int& port)
		{
			const auto sep = address.rfind(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			ip = address.substr(0, sep);
			port = atoi(address.substr(sep + 1).data());
			return !ip.empty() && port > 0;
		}

		std::string mode_short_key(const game::eModes mode)
		{
			switch (mode)
			{
			case game::MODE_MULTIPLAYER:
				return "mp";
			case game::MODE_ZOMBIES:
				return "zm";
			case game::MODE_CAMPAIGN:
				return "sp";
			default:
				return {};
			}
		}

		std::string get_discord_launch_command()
		{
			const auto self = utils::nt::library::get_by_address(get_discord_launch_command);
			if (!self.is_valid())
			{
				return {};
			}

			const auto self_path = self.get_path().string();
			return utils::string::va("\"%s\" -launch \"%%1\"", self_path.data());
		}

		presence_snapshot build_menu_presence()
		{
			presence_snapshot snapshot{};
			snapshot.start_timestamp = start_time;
			snapshot.details = "BOIII";
			snapshot.state = game::Com_IsRunningUILevel() ? "Main Menu" : "Loading...";
			snapshot.large_image_key = "logo";
			snapshot.large_image_text = "BOIII";
			return snapshot;
		}

		presence_snapshot build_game_presence()
		{
			if (!was_in_game)
			{
				was_in_game = true;
				match_time = unix_time();
			}

			const auto data = discord::get_presence_state();

			const auto mode = static_cast<game::eModes>(game::Com_SessionMode_GetMode());
			const auto mode_name = game::get_mode_display_name(mode);
			const auto display_map = data.mapname.empty() ? std::string(mode_name) : data.map_display;
			const auto join_address = get_join_address();

			presence_snapshot snapshot{};
			snapshot.start_timestamp = match_time;

			snapshot.details = data.gametype.empty() ? std::string(mode_name) : data.gametype;
			snapshot.state = display_map;

			snapshot.details = utils::string::truncate(strip_colors(snapshot.details), 128);
			snapshot.state = utils::string::truncate(strip_colors(snapshot.state), 128);
			snapshot.large_image_key = !data.mapname.empty() && data.map_display != data.mapname ? data.mapname : "logo";
			snapshot.large_image_text = utils::string::truncate(std::string(mode_name) + " - " + display_map, 128);
			snapshot.small_image_key = "logo";
			snapshot.small_image_text = "BOIII";

			if (data.max_players > 0)
			{
				snapshot.party_size = data.players;
				snapshot.party_max = data.max_players;
			}

			snapshot.join_secret = make_join_secret(join_address);
			if (!snapshot.join_secret.empty())
			{
				snapshot.party_id = utils::string::va("boiii-%08X", utils::cryptography::fnv1a::compute(join_address));
				snapshot.party_privacy = DISCORD_PARTY_PUBLIC;
			}

			return snapshot;
		}

		void publish_presence(const presence_snapshot& snapshot)
		{
			const auto now = std::chrono::steady_clock::now();
			if (last_presence && *last_presence == snapshot && now - last_presence_update < PRESENCE_HEARTBEAT)
			{
				return;
			}

			DiscordRichPresence presence{};
			ZeroMemory(&presence, sizeof(presence));
			presence.instance = 1;
			presence.details = snapshot.details.data();
			presence.state = snapshot.state.data();
			presence.startTimestamp = snapshot.start_timestamp;
			presence.largeImageKey = snapshot.large_image_key.data();
			presence.largeImageText = snapshot.large_image_text.data();
			presence.smallImageKey = snapshot.small_image_key.data();
			presence.smallImageText = snapshot.small_image_text.data();
			presence.partyId = snapshot.party_id.data();
			presence.partySize = snapshot.party_size;
			presence.partyMax = snapshot.party_max;
			presence.partyPrivacy = snapshot.party_privacy;
			presence.joinSecret = snapshot.join_secret.data();

			Discord_UpdatePresence(&presence);

			last_presence = snapshot;
			last_presence_update = now;
		}

		// Drives the native<->silent handoff with a debounce on the launcher->client direction so a
		// launcher restart doesn't flicker the card through "Game -> gone -> CB Launcher".
		void ownership_tick()
		{
			if (wire_launcher_owns.load())
			{
				release_pending = false;
				if (!effective_launcher_owns)
				{
					effective_launcher_owns = true;
					Discord_ClearPresence(); // clear once on entry; keep the connection initialized
				}
				return;
			}

			if (!effective_launcher_owns)
			{
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			if (!release_pending)
			{
				release_pending = true;
				release_deadline = now + OWNERSHIP_RELEASE_GRACE;
				return;
			}

			if (now >= release_deadline)
			{
				effective_launcher_owns = false;
				release_pending = false;
				last_presence.reset(); // force a repaint when native RPC resumes
			}
		}

		void update_discord()
		{
			if (effective_launcher_owns)
			{
				return; // launcher owns presence; stay silent but connected
			}

			try
			{
				if (!game::Com_IsInGame())
				{
					was_in_game = false;
					publish_presence(build_menu_presence());
					return;
				}

				publish_presence(build_game_presence());
			}
			catch (...)
			{
			}
		}

		void ready(const DiscordUser* request)
		{
			SetEnvironmentVariableA("discord_user", request->userId);
			printf("Discord: Ready: %s - %s\n", request->userId, request->username);
		}

		void errored(const int error_code, const char* message)
		{
			printf("Discord: Error (%i): %s\n", error_code, message);
		}

		void join_game(const char* join_secret)
		{
			if (!join_secret || !join_secret[0])
			{
				return;
			}

			std::string token;
			std::string address;
			if (!parse_join_secret(join_secret, &token, &address))
			{
				// Legacy/raw-address invite (pre-token secrets were just "ip:port").
				const auto parsed = network::address_from_string(join_secret);
				if (!network::is_connectable_address(parsed))
				{
					printf("Discord: invalid join secret\n");
					return;
				}

				token = "-";
				address = network::address_to_string(parsed);
			}

			// Queue like launcher joins; process_pending_route routes once join_ready().
			queue_join(token, address);
		}
	}

	presence_state get_presence_state()
	{
		presence_state state{};
		state.in_game = game::Com_IsInGame();
		if (!state.in_game)
		{
			return state; // menu => empty map
		}

		auto mapname = game::get_dvar_string("mapname");
		if (mapname == "core_frontend")
		{
			mapname.clear();
		}

		auto gametype = game::get_dvar_string("g_gametype");
		if (gametype.empty() || gametype == "frontend")
		{
			gametype = game::get_dvar_string("ui_gametype");
		}

		const auto mode = static_cast<game::eModes>(game::Com_SessionMode_GetMode());

		state.mode = mode_short_key(mode);
		state.mapname = mapname;
		state.map_display = mapname.empty()
			                    ? std::string{}
			                    : utils::string::truncate(strip_colors(game::get_map_display_name(mapname)), 128);
		state.gametype = utils::string::truncate(strip_colors(game::get_gametype_display_name(gametype)), 128);
		state.server_name = utils::string::truncate(strip_colors(party::get_public_server_name()), 128);

		const auto max_clients = getinfo::get_max_client_count();
		if (max_clients > 0)
		{
			auto players = static_cast<int>(getinfo::get_client_count());
			if (players < 1)
			{
				players = 1;
			}
			state.players = players;
			state.max_players = max_clients;
		}

		return state;
	}

	std::optional<join_transport> get_join_transport()
	{
		const auto address = get_join_address();
		if (address.empty())
		{
			return std::nullopt;
		}

		const auto token = nat::current_token();

		join_transport transport{};
		if (token.empty())
		{
			// Directly reachable public server: advertise it as a direct connect.
			if (!split_address(address, transport.ip, transport.port))
			{
				return std::nullopt;
			}
			transport.is_nat = false;
			return transport;
		}

		// Hosting: NAT punch with the reachable endpoint as fallback.
		transport.is_nat = true;
		transport.token = token;
		nat::get_rendezvous(transport.rendezvous_host, transport.rendezvous_port);
		split_address(address, transport.fallback_ip, transport.fallback_port);
		return transport;
	}

	void queue_join(const std::string& token, const std::string& address)
	{
		std::lock_guard lock(pending_route_mutex);
		pending_route = std::make_pair(token, address);
	}

	void set_launcher_presence_owner(const bool launcher_owns)
	{
		wire_launcher_owns.store(launcher_owns);
	}

	class component final : public client_component
	{
	public:
		void post_load() override
		{
			start_time = unix_time();
			game::load_display_names();

			const auto launch_command = get_discord_launch_command();
			if (!launch_command.empty())
			{
				Discord_Register(DISCORD_APP_ID, launch_command.data());
			}

			DiscordEventHandlers handlers{};
			ZeroMemory(&handlers, sizeof(handlers));
			handlers.ready = ready;
			handlers.errored = errored;
			handlers.disconnected = errored;
			handlers.joinGame = join_game;

			Discord_Initialize(DISCORD_APP_ID, &handlers, 0, nullptr);
			this->initialized_ = true;

			scheduler::loop(update_discord, scheduler::pipeline::main, 2s);

			// Callbacks and join routing drive the NAT punch and game socket; main thread only.
			scheduler::loop([]
			{
				ownership_tick();
				Discord_RunCallbacks();
				process_pending_route();
			}, scheduler::pipeline::main, 250ms);
		}

		void pre_destroy() override
		{
			if (this->initialized_)
			{
				Discord_ClearPresence();
				Discord_Shutdown();
			}
		}

	private:
		bool initialized_ = false;
	};
}

REGISTER_COMPONENT(discord::component)
