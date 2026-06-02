#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/utils.hpp"

#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include <discord_register.h>
#include <discord_rpc.h>

#include <utils/cryptography.hpp>
#include <utils/http.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

namespace discord
{
	namespace
	{
		constexpr auto* DISCORD_APP_ID = "1494165323543478392";
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

		std::mutex public_ip_mutex;
		std::string cached_public_ip;
		std::atomic_bool public_ip_fetched{false};

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

		bool is_valid_join_address(const game::netadr_t& address)
		{
			return network::is_ip_address(address)
				&& address.addr != 0
				&& address.ipv4.a != 0
				&& address.ipv4.a != 127
				&& address.ipv4.a < 224
				&& address.port >= 1024;
		}

		uint16_t get_local_port()
		{
			const auto port = game::get_dvar_int("net_port");
			if (port >= 1024 && port <= 65535)
			{
				return static_cast<uint16_t>(port);
			}

			return 3074;
		}

		std::string make_address(const std::string& host, const uint16_t port)
		{
			if (host.empty() || port < 1024)
			{
				return {};
			}

			const auto candidate = utils::string::va("%s:%hu", host.data(), port);
			const auto parsed = network::address_from_string(candidate);
			if (!is_valid_join_address(parsed))
			{
				return {};
			}

			return network::address_to_string(parsed);
		}

		std::string get_preferred_local_ip()
		{
			ULONG length = 15000;
			std::vector<unsigned char> buffer(length);
			auto* adapter_info = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());

			if (GetAdaptersInfo(adapter_info, &length) == ERROR_BUFFER_OVERFLOW)
			{
				buffer.resize(length);
				adapter_info = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
			}

			if (GetAdaptersInfo(adapter_info, &length) != NO_ERROR)
			{
				return {};
			}

			std::string radmin_ip;
			std::string hamachi_ip;

			for (auto* adapter = adapter_info; adapter; adapter = adapter->Next)
			{
				const std::string ip = adapter->IpAddressList.IpAddress.String;
				if (ip == "0.0.0.0" || ip.empty())
				{
					continue;
				}

				if (utils::string::starts_with(ip, "26."))
				{
					radmin_ip = ip;
				}
				else if (utils::string::starts_with(ip, "25."))
				{
					hamachi_ip = ip;
				}
			}

			if (!radmin_ip.empty())
			{
				return radmin_ip;
			}

			return hamachi_ip;
		}

		void fetch_public_ip()
		{
			try
			{
				auto response = utils::http::get_data("https://api.ipify.org", {}, {}, 1);
				if (!response || response->empty())
				{
					return;
				}

				utils::string::trim(*response);
				const auto public_ip = network::address_from_string(*response, true);
				if (!network::is_valid_public_ip(public_ip))
				{
					return;
				}

				std::lock_guard lock(public_ip_mutex);
				cached_public_ip = *response;
				public_ip_fetched = true;
			}
			catch (...)
			{
			}
		}

		std::string get_cached_public_ip()
		{
			if (!public_ip_fetched)
			{
				return {};
			}

			std::lock_guard lock(public_ip_mutex);
			return cached_public_ip;
		}

		std::string get_udp_local_ip()
		{
			std::string local_ip;
			const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (sock == INVALID_SOCKET)
			{
				return {};
			}

			sockaddr_in target{};
			target.sin_family = AF_INET;
			target.sin_port = htons(53);
			inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);

			if (connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0)
			{
				sockaddr_in local{};
				int length = sizeof(local);
				if (getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) == 0)
				{
					char buffer[INET_ADDRSTRLEN]{};
					inet_ntop(AF_INET, &local.sin_addr, buffer, sizeof(buffer));
					local_ip = buffer;
				}
			}

			closesocket(sock);
			return local_ip;
		}

		std::string resolve_private_or_loopback_address(const uint16_t port)
		{
			const auto vpn_ip = get_preferred_local_ip();
			if (!vpn_ip.empty())
			{
				const auto address = make_address(vpn_ip, port);
				if (!address.empty())
				{
					return address;
				}
			}

			const auto public_ip = get_cached_public_ip();
			if (!public_ip.empty())
			{
				const auto address = make_address(public_ip, port);
				if (!address.empty())
				{
					return address;
				}
			}

			const auto local_ip = get_udp_local_ip();
			if (!local_ip.empty())
			{
				return make_address(local_ip, port);
			}

			return {};
		}

		std::string get_join_address()
		{
			if (!game::Com_IsInGame())
			{
				return {};
			}

			const auto connected = party::get_connected_server();

			if (is_valid_join_address(connected))
			{
				if (!network::is_private_ip(connected))
				{
					return network::address_to_string(connected);
				}

				if (const auto public_ip = get_cached_public_ip(); !public_ip.empty())
				{
					if (const auto address = make_address(public_ip, connected.port); !address.empty())
					{
						return address;
					}
				}

				return network::address_to_string(connected);
			}

			return resolve_private_or_loopback_address(get_local_port());
		}

		std::string make_join_secret(const std::string& address)
		{
			if (address.empty())
			{
				return {};
			}

			const auto secret = std::string(JOIN_SECRET_PREFIX) + address;
			if (secret.size() >= 128)
			{
				return {};
			}

			return secret;
		}

		bool parse_join_secret(const std::string& secret, std::string* address)
		{
			if (!utils::string::starts_with(secret, JOIN_SECRET_PREFIX))
			{
				return false;
			}

			const auto raw_address = secret.substr(strlen(JOIN_SECRET_PREFIX));
			const auto parsed = network::address_from_string(raw_address);
			if (!is_valid_join_address(parsed))
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

			std::string address;
			if (!parse_join_secret(secret, &address))
			{
				show_join_error();
				return;
			}

			game::Cbuf_AddText(0, utils::string::va("connect %s\n", address.data()));
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

		int get_client_count(const int max_clients)
		{
			int count = 0;
			char name[64]{};

			for (int i = 0; i < max_clients; ++i)
			{
				name[0] = '\0';
				if (game::CL_GetClientName(0, i, name, sizeof(name), false) && name[0] != '\0')
				{
					++count;
				}
			}

			return count;
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

			const auto max_clients = game::get_dvar_int("com_maxclients");
			if (max_clients > 0)
			{
				snapshot.party_size = get_client_count(max_clients);
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

			scheduler::once(fetch_public_ip, scheduler::pipeline::async, 2s);
			scheduler::loop(fetch_public_ip, scheduler::pipeline::async, 30min);
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
