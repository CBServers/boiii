#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/utils.hpp"

#include "command.hpp"
#include "getinfo.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "scheduler.hpp"

#include <utils/string.hpp>

#include <random>
#include <sstream>

namespace nat
{
	namespace
	{
		const game::dvar_t* rendezvous_ip{};
		const game::dvar_t* rendezvous_port{};

		// All state below is touched only on the main thread, so no locking is needed.
		std::string host_token{}; // non-empty while hosting
		std::string observed_public_endpoint{}; // our public endpoint, reflected by the rendezvous

		struct punch_attempt
		{
			bool active = false;
			bool joining = false; // true => we issue "connect" on success
			bool connected = false;
			std::string token{};
			std::string fallback_address{}; // joiner-only: tried on timeout
			std::vector<game::netadr_t> candidates{};
			std::chrono::steady_clock::time_point deadline{};
		};

		punch_attempt punch{};

		bool get_rendezvous_server(game::netadr_t& address)
		{
			const auto* ip = rendezvous_ip->current.value.string;
			const auto* port = rendezvous_port->current.value.string;
			address = network::address_from_string(utils::string::va("%s:%s", ip, port));
			return address.type != game::NA_BAD;
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

			const auto parsed = network::address_from_string(utils::string::va("%s:%hu", host.data(), port));
			if (!network::is_connectable_address(parsed))
			{
				return {};
			}

			return network::address_to_string(parsed);
		}

		std::string get_local_candidate()
		{
			std::string ip;
			const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (sock != INVALID_SOCKET)
			{
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
						ip = buffer;
					}
				}

				closesocket(sock);
			}

			if (ip.empty())
			{
				return {};
			}

			return ip + ":" + std::to_string(get_local_port());
		}

		// A Radmin (26.x) / Hamachi (25.x) address, if present; these VPNs bypass NAT.
		std::string get_vpn_candidate()
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

			const auto& vpn_ip = !radmin_ip.empty() ? radmin_ip : hamachi_ip;
			return vpn_ip.empty() ? std::string{} : make_address(vpn_ip, get_local_port());
		}

		std::vector<std::string> gather_candidates()
		{
			std::vector<std::string> candidates;
			if (auto lan = get_local_candidate(); !lan.empty())
			{
				candidates.push_back(std::move(lan));
			}

			if (auto vpn = get_vpn_candidate(); !vpn.empty())
			{
				candidates.push_back(std::move(vpn));
			}

			return candidates;
		}

		std::string generate_token()
		{
			static constexpr char hex[] = "0123456789abcdef";
			std::random_device rd;
			std::mt19937_64 gen(rd());
			std::uniform_int_distribution<int> dist(0, 15);

			std::string token;
			token.reserve(16);
			for (int i = 0; i < 16; ++i)
			{
				token.push_back(hex[dist(gen)]);
			}

			return token;
		}

		std::vector<std::string> split_ws(const std::string& text)
		{
			std::vector<std::string> out;
			std::istringstream stream(text);
			std::string token;
			while (stream >> token)
			{
				out.push_back(token);
			}

			return out;
		}

		void send_to_rendezvous(const std::string& command, const std::string& token)
		{
			game::netadr_t addr{};
			if (!get_rendezvous_server(addr))
			{
				printf("[nat] could not resolve rendezvous server\n");
				return;
			}

			auto data = token;
			for (const auto& candidate : gather_candidates())
			{
				data += " " + candidate;
			}

			network::send(addr, command, data);
		}

		void add_candidate(const game::netadr_t& address)
		{
			if (!network::is_ip_address(address))
			{
				return;
			}

			for (const auto& existing : punch.candidates)
			{
				if (network::are_addresses_equal(existing, address))
				{
					return;
				}
			}

			punch.candidates.push_back(address);
		}

		void send_punch_round()
		{
			for (const auto& candidate : punch.candidates)
			{
				network::send(candidate, "punch", punch.token);
			}
		}

		void issue_connect(const std::string& address)
		{
			printf("[nat] connecting to %s\n", address.data());
			game::Cbuf_AddText(0, utils::string::va("connect %s\n", address.data()));
		}

		void show_join_error()
		{
			game::UI_OpenErrorPopupWithMessage(0, game::ERROR_UI,
				"Could not reach the host. They may be on a restricted network (e.g. a mobile "
				"hotspot). Ask them to host on home Wi-Fi, port-forward, or use a VPN like Radmin.");
		}

		void feed_candidates(const std::vector<std::string>& candidate_strings)
		{
			for (const auto& candidate : candidate_strings)
			{
				add_candidate(network::address_from_string(candidate));
			}
		}

		void punch_frame()
		{
			if (!punch.active)
			{
				return;
			}

			if (std::chrono::steady_clock::now() > punch.deadline)
			{
				if (!punch.connected && punch.joining)
				{
					if (!punch.fallback_address.empty())
					{
						printf("[nat] no direct path; trying fallback %s\n", punch.fallback_address.data());
						issue_connect(punch.fallback_address);
					}
					else
					{
						printf("[nat] join failed for token=%s (no direct path)\n", punch.token.data());
						show_join_error();
					}
				}

				punch.active = false;
				return;
			}

			send_punch_round();
		}

		// is_host() excludes the frontend menu, where sv_running is also true.
		void update_host_session()
		{
			if (getinfo::is_host())
			{
				if (host_token.empty())
				{
					host_token = generate_token();
					printf("[nat] hosting private match, session token=%s\n", host_token.data());
				}

				send_to_rendezvous("privRegister", host_token); // register + keepalive
			}
			else if (!host_token.empty())
			{
				printf("[nat] match ended, dropping session token=%s\n", host_token.data());
				host_token.clear();
				observed_public_endpoint.clear();
			}
		}
	}

	std::string current_token()
	{
		return host_token;
	}

	std::string get_host_endpoint()
	{
		// Gated on host_token so the advertised endpoint and join-secret token agree.
		if (host_token.empty())
		{
			return {};
		}

		// Fallback priority when punching fails: public (port-forward) > VPN > LAN.
		if (!observed_public_endpoint.empty())
		{
			return observed_public_endpoint;
		}

		if (auto vpn = get_vpn_candidate(); !vpn.empty())
		{
			return vpn;
		}

		return get_local_candidate();
	}

	void begin_join(const std::string& token, const std::string& fallback_address)
	{
		punch = punch_attempt{};
		punch.active = true;
		punch.joining = true;
		punch.token = token;
		punch.fallback_address = fallback_address;
		punch.deadline = std::chrono::steady_clock::now() + 12s;

		printf("[nat] joining token=%s (fallback=%s)\n", token.data(),
			fallback_address.empty() ? "none" : fallback_address.data());
		send_to_rendezvous("privJoin", token);
	}

	class component final : public client_component
	{
	public:
		void post_unpack() override
		{
			scheduler::once([]
			{
				rendezvous_ip = game::register_dvar_string("rendezvousServerIP", "us.cbservers.xyz",
					game::DVAR_NONE, "IP of the private-game rendezvous server");
				rendezvous_port = game::register_dvar_string("rendezvousServerPort", "20810",
					game::DVAR_NONE, "Port of the private-game rendezvous server");
			}, scheduler::pipeline::main);

			network::on("privRegisterAck", [](const game::netadr_t&, const network::data_view& data)
			{
				// The rendezvous reflects our observed public endpoint (STUN-style).
				const std::string payload(reinterpret_cast<const char*>(data.data()), data.size());
				if (const auto parsed = network::address_from_string(payload);
					network::is_connectable_address(parsed))
				{
					observed_public_endpoint = network::address_to_string(parsed);
				}
			});

			// privPeer payload: "<token> <cand1> <cand2> ..."
			network::on("privPeer", [](const game::netadr_t&, const network::data_view& data)
			{
				const std::string payload(reinterpret_cast<const char*>(data.data()), data.size());
				const auto fields = split_ws(payload);
				if (fields.empty())
				{
					return;
				}

				const auto& token = fields[0];
				const std::vector<std::string> candidates(fields.begin() + 1, fields.end());

				if (punch.active && punch.joining && punch.token == token)
				{
					// Joiner: feed the host's candidates into our active attempt.
					feed_candidates(candidates);
					send_punch_round();
				}
				else if (!host_token.empty() && token == host_token)
				{
					// Host: punch toward the joiner so our NAT opens. No connect.
					punch = punch_attempt{};
					punch.active = true;
					punch.joining = false;
					punch.token = token;
					punch.deadline = std::chrono::steady_clock::now() + 10s;
					feed_candidates(candidates);
					send_punch_round();
				}
			});

			network::on("privReject", [](const game::netadr_t&, const network::data_view& data)
			{
				const std::string reason(reinterpret_cast<const char*>(data.data()), data.size());
				printf("[nat] privReject: %s\n", reason.data());
				if (punch.active && punch.joining)
				{
					// Session gone: fall straight to the direct path if we have one.
					punch.deadline = std::chrono::steady_clock::now();
				}
			});

			// Ack the observed source address, not the claimed one (symmetric NAT).
			network::on("punch", [](const game::netadr_t& from, const network::data_view& data)
			{
				const std::string token(reinterpret_cast<const char*>(data.data()), data.size());
				network::send(from, "punchAck", token);

				if (punch.active && punch.token == token)
				{
					add_candidate(from);
				}
			});

			network::on("punchAck", [](const game::netadr_t& from, const network::data_view& data)
			{
				const std::string token(reinterpret_cast<const char*>(data.data()), data.size());
				if (!punch.active || punch.token != token || punch.connected)
				{
					return;
				}

				punch.connected = true;
				punch.active = false;

				const auto endpoint = network::address_to_string(from);
				if (punch.joining)
				{
					printf("[nat] direct path open to %s\n", endpoint.data());
					issue_connect(endpoint);
				}
				else
				{
					printf("[nat] host path open to %s\n", endpoint.data());
				}
			});

			scheduler::loop(punch_frame, scheduler::pipeline::main, 250ms);

			// Host session register/keepalive/teardown, driven purely by game state.
			scheduler::loop(update_host_session, scheduler::pipeline::main, 5s);

			// Manual join for debugging (hosting is automatic now).
			command::add("nat_join", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					printf("[nat] usage: nat_join <token>\n");
					return;
				}

				begin_join(params.get(1), {});
			});
		}
	};
}

REGISTER_COMPONENT(nat::component)
