#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/utils.hpp"

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

		std::mutex pending_join_mutex;
		std::string pending_join_secret;

		int64_t start_time = 0;
		int64_t match_time = 0;
		bool was_in_game = false;

		std::optional<presence_snapshot> last_presence;
		std::chrono::steady_clock::time_point last_presence_update{};

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

			// Client on a directly-reachable server: advertise it (token will be "-").
			const auto connected = party::get_connected_server();
			if (network::is_connectable_address(connected) && !network::is_private_ip(connected))
			{
				return network::address_to_string(connected);
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

		void show_join_error()
		{
			game::UI_OpenErrorPopupWithMessage(0, game::ERROR_UI,
				"Discord join failed. The activity invite is no longer valid.");
		}

		void process_pending_join()
		{
			std::string secret;
			{
				std::lock_guard lock(pending_join_mutex);
				secret = std::move(pending_join_secret);
				pending_join_secret.clear();
			}

			if (secret.empty())
			{
				return;
			}

			std::string token;
			std::string address;
			if (!parse_join_secret(secret, &token, &address))
			{
				show_join_error();
				return;
			}

			// "-" / empty token => friend is on a directly reachable server.
			if (token.empty() || token == "-")
			{
				game::Cbuf_AddText(0, utils::string::va("connect %s\n", address.data()));
				return;
			}

			// Hole-punch toward the host; falls back to `address` (port-forward/VPN).
			nat::begin_join(token, address);
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
			const auto mode_name = game::get_mode_display_name(mode);
			const auto display_map = mapname.empty() ? mode_name : game::get_map_display_name(mapname);
			const auto display_gametype = game::get_gametype_display_name(gametype);
			const auto join_address = get_join_address();

			presence_snapshot snapshot{};
			snapshot.start_timestamp = match_time;

			snapshot.details = display_gametype.empty() ? mode_name : display_gametype;
			snapshot.state = display_map;

			snapshot.details = utils::string::truncate(strip_colors(snapshot.details), 128);
			snapshot.state = utils::string::truncate(strip_colors(snapshot.state), 128);
			snapshot.large_image_key = !mapname.empty() && game::get_map_display_name(mapname) != mapname ? mapname : "logo";
			snapshot.large_image_text = utils::string::truncate(std::string(mode_name) + " - " + display_map, 128);
			snapshot.small_image_key = "logo";
			snapshot.small_image_text = "BOIII";

			const auto max_clients = getinfo::get_max_client_count();
			if (max_clients > 0)
			{
				snapshot.party_size = static_cast<int>(getinfo::get_client_count());
				if (snapshot.party_size < 1)
				{
					snapshot.party_size = 1;
				}

				snapshot.party_max = max_clients;
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

		void update_discord()
		{
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

			std::lock_guard lock(pending_join_mutex);
			pending_join_secret = join_secret;
		}
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

			scheduler::loop(Discord_RunCallbacks, scheduler::pipeline::async, 1s);
			scheduler::loop(update_discord, scheduler::pipeline::main, 5s);
			scheduler::loop(process_pending_join, scheduler::pipeline::main, 250ms);
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
