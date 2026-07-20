#include <std_include.hpp>

#include "friends.hpp"
#include "ipc.hpp"
#include "nat.hpp"
#include "scheduler.hpp"

#include <utils/concurrency.hpp>
#include <utils/string.hpp>

// Store for CB-Launcher friends fed over IPC (see FRIENDS_IPC_HANDOFF.md §9). The ISteamFriends
// stubs in steam/interfaces/friends.cpp render this into the native Social UI.
namespace friends
{
	namespace
	{
		struct store_t
		{
			std::vector<friend_record> list;
			std::unordered_map<std::string, unsigned int> account_ids; // discord id -> sticky account id
			unsigned int next_account_id{1000001};
			std::unordered_map<std::string, int> map_ids;
			std::unordered_map<std::string, int> gametype_ids;
			int version{0};
		};

		utils::concurrency::container<store_t>& get_store()
		{
			static utils::concurrency::container<store_t> store;
			return store;
		}

		// steam_id with account_instance=1, account_type=1 (individual), universe=1 (public)
		unsigned long long make_steam_id_bits(const unsigned int account_id)
		{
			return (1ull << 56) | (1ull << 52) | (1ull << 32) | account_id;
		}

		int map_persona_state(const std::string& status)
		{
			if (status == "online") return 1;
			if (status == "dnd") return 2;
			if (status == "idle") return 3;
			return 0;
		}
	}

	void apply_snapshot(const std::vector<snapshot_entry>& entries)
	{
		get_store().access([&](store_t& store)
		{
			std::vector<friend_record> list;
			list.reserve(entries.size());

			for (const auto& entry : entries)
			{
				if (entry.discord_id.empty())
				{
					continue;
				}

				auto id_it = store.account_ids.find(entry.discord_id);
				if (id_it == store.account_ids.end())
				{
					id_it = store.account_ids.emplace(entry.discord_id, store.next_account_id++).first;
				}

				friend_record record{};
				record.steam_id_bits = make_steam_id_bits(id_it->second);
				record.discord_id = entry.discord_id;
				record.name = entry.name.empty() ? entry.discord_id : entry.name;
				record.persona_state = map_persona_state(entry.status);
				record.in_game = entry.has_game && entry.game_id == "boiii";
				record.joinable = record.in_game && entry.joinable;
				record.mode = entry.mode;
				record.map = entry.map;
				record.gametype = entry.gametype;
				list.push_back(std::move(record));
			}

			store.list = std::move(list);
			store.version++;
		});

		printf("[friends] snapshot applied: %zu friends\n", entries.size());
	}

	int snapshot_version()
	{
		return get_store().access<int>([](const store_t& store)
		{
			return store.version;
		});
	}

	std::vector<friend_record> get_friends()
	{
		return get_store().access<std::vector<friend_record>>([](const store_t& store)
		{
			return store.list;
		});
	}

	bool find_friend(const unsigned long long steam_id_bits, friend_record& out)
	{
		return get_store().access<bool>([&](const store_t& store)
		{
			for (const auto& record : store.list)
			{
				if (record.steam_id_bits == steam_id_bits)
				{
					out = record;
					return true;
				}
			}
			return false;
		});
	}

	bool is_joinable(const unsigned long long steam_id_bits)
	{
		friend_record record{};
		return find_friend(steam_id_bits, record) && record.joinable;
	}

	void register_map_id(const std::string& mapname, const int unique_id)
	{
		get_store().access([&](store_t& store)
		{
			store.map_ids[mapname] = unique_id;
		});
	}

	void register_gametype_id(const std::string& gametype, const int id)
	{
		get_store().access([&](store_t& store)
		{
			store.gametype_ids[gametype] = id;
		});
	}

	int lookup_map_id(const std::string& mapname)
	{
		return get_store().access<int>([&](const store_t& store)
		{
			const auto it = store.map_ids.find(mapname);
			return it == store.map_ids.end() ? -1 : it->second;
		});
	}

	int lookup_gametype_id(const std::string& gametype)
	{
		return get_store().access<int>([&](const store_t& store)
		{
			const auto it = store.gametype_ids.find(gametype);
			return it == store.gametype_ids.end() ? -1 : it->second;
		});
	}

	bool request_join(const unsigned long long steam_id_bits)
	{
		friend_record record{};
		if (!find_friend(steam_id_bits, record) || record.discord_id.empty())
		{
			printf("[friends] request_join: no friend for %llX\n", steam_id_bits);
			return false;
		}

		ipc::send_message(::utils::string::va(R"({"type":"join-friend","friendId":"%s"})",
		                                      record.discord_id.data()));
		printf("[friends] join-friend sent for %s (%s)\n", record.name.data(), record.discord_id.data());
		return true;
	}

	bool request_invite(const unsigned long long steam_id_bits)
	{
		friend_record record{};
		if (!find_friend(steam_id_bits, record) || record.discord_id.empty())
		{
			printf("[friends] request_invite: no friend for %llX\n", steam_id_bits);
			return false;
		}

		// Inviting is host consent: open a closed private match so the invite can carry a real secret.
		scheduler::once([]
		{
			if (nat::can_open_to_friends() && nat::open_to_friends())
			{
				ipc::flush_presence();
			}
		}, scheduler::main);

		ipc::send_message(::utils::string::va(R"({"type":"invite","friendId":"%s"})",
		                                      record.discord_id.data()));
		printf("[friends] invite sent for %s (%s)\n", record.name.data(), record.discord_id.data());
		return true;
	}
}
