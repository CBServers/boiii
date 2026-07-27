#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/utils.hpp"

#include "command.hpp"
#include "getinfo.hpp"
#include "ipc.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include <utils/cryptography.hpp>
#include <utils/string.hpp>

#include <atomic>
#include <mutex>

namespace nat
{
	namespace
	{
		const game::dvar_t* rendezvous_ip{};
		const game::dvar_t* rendezvous_port{};

		// How long the joined match identity outlives a load screen before it's considered stale.
		constexpr auto JOINED_TOKEN_GRACE = 15s;

		// Re-advertising the host's session needs its live open state AND current token (closing
		// drops it, reopening mints a fresh one); the connect-time server info is a one-shot.
		constexpr auto JOINED_HOST_PROBE_INTERVAL = 5s;
		constexpr auto JOINED_HOST_OPEN_TTL = 20s; // unanswered probes stop the advertising too

		// Sent in place of a token when the host is no longer open to friends.
		constexpr auto SESSION_CLOSED = "-";

		// Tokens and challenges are both get_challenge() output: 16 uppercase hex chars.
		constexpr size_t TOKEN_LENGTH = 16;

		// State below is touched only on the main thread (no locking) unless noted otherwise.
		bool hosting_enabled{}; // host opted in via nat_host; mirrored into the nat_open dvar
		std::string host_token{}; // non-empty while hosting
		std::string hosted_token{}; // last host_token, retained across a close so the match keeps its identity
		std::string joined_token{}; // token of the punched session we joined; cleared when we leave
		std::chrono::steady_clock::time_point joined_token_deadline{};
		std::chrono::steady_clock::time_point joined_host_open_until{}; // host still accepts joins
		std::string session_challenge{}; // outstanding natSession challenge, empty when none
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
			std::chrono::steady_clock::time_point next_rendezvous_retry{}; // joiner: privJoin until candidates arrive
		};

		punch_attempt punch{};

		// The rendezvous DNS result, resolved off-thread; guarded by rendezvous_mutex.
		std::mutex rendezvous_mutex;
		std::string rendezvous_key;     // "host:port" the cache was resolved for
		std::string rendezvous_numeric; // resolved "ip:port", empty if resolution failed
		std::atomic_bool rendezvous_resolving{false};

		// Blocking; async pipeline only.
		std::string resolve_ipv4(const std::string& host)
		{
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;

			addrinfo* result = nullptr;
			if (getaddrinfo(host.data(), nullptr, &hints, &result) != 0 || !result)
			{
				return {};
			}

			char buffer[INET_ADDRSTRLEN]{};
			inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr, buffer, sizeof(buffer));
			freeaddrinfo(result);
			return buffer;
		}

		// Non-blocking: uses the cached DNS result and kicks an async resolve when it's missing/stale.
		bool get_rendezvous_server(game::netadr_t& address)
		{
			if (!rendezvous_ip || !rendezvous_port)
			{
				return false;
			}

			const std::string key = utils::string::va("%s:%s",
				rendezvous_ip->current.value.string, rendezvous_port->current.value.string);

			std::string numeric;
			{
				std::lock_guard<std::mutex> lock(rendezvous_mutex);
				if (rendezvous_key == key)
				{
					numeric = rendezvous_numeric;
				}
			}

			if (numeric.empty())
			{
				if (!rendezvous_resolving.exchange(true))
				{
					scheduler::once([key]
					{
						const auto sep = key.rfind(':');
						const auto ip = resolve_ipv4(key.substr(0, sep));

						std::lock_guard<std::mutex> lock(rendezvous_mutex);
						rendezvous_key = key;
						rendezvous_numeric = ip.empty() ? std::string{} : ip + key.substr(sep);
						rendezvous_resolving = false;
					}, scheduler::pipeline::async);
				}
				return false;
			}

			address = network::address_from_string(numeric);
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

		struct endpoint_candidates
		{
			std::string lan;
			std::string vpn;
		};

		// LAN/VPN probes (socket + adapter enumeration) are syscall-heavy; refresh at most once a minute.
		const endpoint_candidates& get_candidates()
		{
			static endpoint_candidates cached{};
			static std::chrono::steady_clock::time_point expiry{};

			const auto now = std::chrono::steady_clock::now();
			if (now >= expiry)
			{
				cached.lan = get_local_candidate();
				cached.vpn = get_vpn_candidate();
				expiry = now + 60s;
			}

			return cached;
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
			const auto& candidates = get_candidates();
			if (!candidates.lan.empty())
			{
				data += " " + candidates.lan;
			}
			if (!candidates.vpn.empty())
			{
				data += " " + candidates.vpn;
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

			// party::connect directly; skips a command-buffer round trip.
			const auto target = network::address_from_string(address);
			if (network::is_connectable_address(target))
			{
				party::connect(target);

				// Adopt the host's token as our match identity only once we actually connect, so a
				// punch that never lands can't mislabel the match we're still sitting in.
				if (punch.joining)
				{
					joined_token = punch.token;
					joined_token_deadline = std::chrono::steady_clock::now() + JOINED_TOKEN_GRACE;
					// The session was open a moment ago; seed the TTL so we aren't mute until the first probe.
					joined_host_open_until = std::chrono::steady_clock::now() + JOINED_HOST_OPEN_TTL;
				}
			}
			else
			{
				printf("[nat] refusing to connect to unconnectable address %s\n", address.data());
			}
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
				if (candidate.empty())
				{
					continue;
				}

				add_candidate(network::address_from_string(candidate));
			}
		}

		// Retires the joined session's identity once we're out of a match (there is no disconnect
		// hook). Grace-period based: map changes drop Com_IsInGame briefly, and losing the token on
		// every load screen would silently disable same-match detection for the rest of the session.
		void update_joined_session()
		{
			if (joined_token.empty())
			{
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			if (punch.active || game::Com_IsInGame())
			{
				joined_token_deadline = now + JOINED_TOKEN_GRACE;
			}
			else if (now >= joined_token_deadline)
			{
				joined_token.clear();
				joined_host_open_until = {};
			}
		}

		// Wire values are constrained to the shape get_challenge() emits, so a hostile host can't
		// inject separators into the launcher's join-secret grammar.
		bool is_valid_token(const std::string& value)
		{
			return value.size() == TOKEN_LENGTH && std::ranges::all_of(value, [](const char c)
			{
				return std::isxdigit(static_cast<unsigned char>(c)) != 0;
			});
		}

		// Asks the host for its live session state, so we stop re-advertising a match it closed and
		// pick up a new token when it reopens (the "CLOSE TO FRIENDS" toggle does both silently).
		// Challenged, so a packet forged from the host's address can't redirect our friends.
		void request_joined_session()
		{
			if (joined_token.empty())
			{
				joined_host_open_until = {};
				session_challenge.clear();
				return;
			}

			const auto host = party::get_connected_server();
			if (!network::is_connectable_address(host))
			{
				return;
			}

			session_challenge = utils::cryptography::random::get_challenge();
			network::send(host, "natSession", session_challenge);
		}

		void punch_frame()
		{
			update_joined_session();

			if (!punch.active)
			{
				return;
			}

			// Retry privJoin (UDP loss / DNS still resolving) until the rendezvous answers with candidates.
			const auto now = std::chrono::steady_clock::now();
			if (punch.joining && punch.candidates.empty() && now >= punch.next_rendezvous_retry)
			{
				send_to_rendezvous("privJoin", punch.token);
				punch.next_rendezvous_retry = now + 1s;
			}

			if (now > punch.deadline)
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

		void set_hosting_enabled(bool enabled)
		{
			hosting_enabled = enabled;
			game::Dvar_SetFromStringByName("nat_open", enabled ? "1" : "0", true);
			// Disable pause-freeze while open
			game::Dvar_SetFromStringByName("com_pauseSupported", enabled ? "0" : "1", true);
			if (!enabled)
			{
				host_token.clear();
				observed_public_endpoint.clear();
			}
		}

		// is_host() excludes the frontend menu, where sv_running is also true.
		void update_host_session()
		{
			const auto hosting = getinfo::is_host();
			if (!hosting)
			{
				// Left the match: the identity dies with it, unlike a mere close to friends.
				hosted_token.clear();
			}

			if (hosting && hosting_enabled)
			{
				if (host_token.empty())
				{
					host_token = utils::cryptography::random::get_challenge();
					hosted_token = host_token;
					printf("[nat] opened private match to friends, token=%s\n", host_token.data());
				}

				send_to_rendezvous("privRegister", host_token); // register + keepalive
			}
			else if (hosting_enabled || !host_token.empty())
			{
				// Toggled off, match ended, or returned to menu: close + reset the toggle.
				if (!host_token.empty())
				{
					printf("[nat] closing private match, dropping token=%s\n", host_token.data());
				}

				set_hosting_enabled(false);
			}
		}
	}

	std::string current_token()
	{
		return host_token;
	}

	std::string hosted_session_token()
	{
		return hosted_token;
	}

	std::string joined_session_token()
	{
		return joined_token;
	}

	bool joined_session_open()
	{
		return !joined_token.empty() && std::chrono::steady_clock::now() < joined_host_open_until;
	}

	bool can_open_to_friends()
	{
		return getinfo::is_host() && !hosting_enabled;
	}

	bool open_to_friends()
	{
		if (!getinfo::is_host())
		{
			return false;
		}

		if (!hosting_enabled)
		{
			set_hosting_enabled(true);
			update_host_session(); // register now, not on the next 5s tick
		}

		return true;
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

		const auto& candidates = get_candidates();
		return !candidates.vpn.empty() ? candidates.vpn : candidates.lan;
	}

	void get_rendezvous(std::string& host, int& port)
	{
		host = rendezvous_ip ? rendezvous_ip->current.value.string : "master.cbservers.xyz";
		port = rendezvous_port ? atoi(rendezvous_port->current.value.string) : 20810;
	}

	void begin_join(const std::string& token, const std::string& fallback_address)
	{
		punch = punch_attempt{};
		punch.active = true;
		punch.joining = true;
		punch.token = token;
		punch.fallback_address = fallback_address;
		punch.deadline = std::chrono::steady_clock::now() + 12s;
		punch.next_rendezvous_retry = std::chrono::steady_clock::now() + 1s;
		joined_token.clear(); // re-adopted in issue_connect once the join actually lands

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
				rendezvous_ip = game::register_dvar_string("rendezvousServerIP", "master.cbservers.xyz",
					game::DVAR_NONE, "IP of the private-game rendezvous server");
				rendezvous_port = game::register_dvar_string("rendezvousServerPort", "20810",
					game::DVAR_NONE, "Port of the private-game rendezvous server");
				(void)game::register_dvar_bool("nat_open", false, game::DVAR_NONE,
					"Allow friends to join this private match");

				game::netadr_t warm{};
				get_rendezvous_server(warm); // kick the async DNS resolve so first use hits the cache
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
				const auto fields = utils::string::split(payload, ' ');
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
					// Host: punch toward the joiner so our NAT opens (no connect); merge if already punching.
					if (!punch.active || punch.joining)
					{
						punch = punch_attempt{};
						punch.active = true;
						punch.joining = false;
						punch.token = token;
					}
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

			// Host: answer a member's session query so it can keep re-advertising us with a live
			// token. Only addresses in our client list get an answer, so the token stays in-match.
			network::on("natSession", [](const game::netadr_t& from, const network::data_view& data)
			{
				const std::string challenge(reinterpret_cast<const char*>(data.data()), data.size());
				if (!is_valid_token(challenge) || !getinfo::is_host())
				{
					return;
				}

				bool is_member = false;
				game::foreach_connected_client([&](const game::client_s& client)
				{
					// Skips bots (NA_BOT) and the local listen-server player (NA_LOOPBACK).
					if (network::is_connectable_address(client.address)
						&& network::are_addresses_equal(client.address, from))
					{
						is_member = true;
					}
				});

				if (!is_member)
				{
					return;
				}

				network::send(from, "natSessionAck",
					challenge + " " + (host_token.empty() ? SESSION_CLOSED : host_token));
			});

			// Member: adopt the host's live token, or stop advertising when it closed.
			network::on("natSessionAck", [](const game::netadr_t& from, const network::data_view& data)
			{
				// While joining, issue_connect owns joined_token; don't race it.
				if (session_challenge.empty() || joined_token.empty() || (punch.active && punch.joining)
					|| !party::is_host(from))
				{
					return;
				}

				const std::string payload(reinterpret_cast<const char*>(data.data()), data.size());
				const auto fields = utils::string::split(payload, ' ');
				if (fields.size() != 2 || fields[0] != session_challenge)
				{
					return;
				}

				session_challenge.clear();

				// Closed: the match identity stays valid, only the re-advertising stops.
				if (fields[1] == SESSION_CLOSED || !is_valid_token(fields[1]))
				{
					joined_host_open_until = {};
					return;
				}

				const auto rotated = fields[1] != joined_token;
				joined_token = fields[1];
				joined_host_open_until = std::chrono::steady_clock::now() + JOINED_HOST_OPEN_TTL;

				if (rotated)
				{
					printf("[nat] host session token rotated to %s\n", joined_token.data());
					ipc::flush_presence(); // republish the secret now instead of on the next tick
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

			// Joined session liveness + token refresh, straight from the host.
			scheduler::loop(request_joined_session, scheduler::pipeline::main, JOINED_HOST_PROBE_INTERVAL);

			// Toggle whether the current private match is open to friends.
			command::add("nat_host", [](const command::params&)
			{
				if (!getinfo::is_host())
				{
					printf("[nat] not hosting a match; cannot open to friends\n");
					return;
				}

				set_hosting_enabled(!hosting_enabled);
				printf("[nat] match is now %s to friends\n", hosting_enabled ? "OPEN" : "CLOSED");

				// Mint the token and register with the rendezvous now instead of on the next 5s tick.
				update_host_session();
			});

			// Manual join for debugging.
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
