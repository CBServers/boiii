#include <std_include.hpp>

#include "resource.hpp"

#include "game.hpp"
#include "utils.hpp"

#include <utils/nt.hpp>

namespace game
{
	static_assert(offsetof(dvar_t, debugName) == 8);
	static_assert(offsetof(dvar_t, description) == 16);
	static_assert(offsetof(dvar_t, flags) == 24);
	static_assert(offsetof(dvar_t, type) == 28);
	static_assert(offsetof(dvar_t, modified) == 32);
	static_assert(offsetof(dvar_t, current) == 40);

	namespace
	{
		struct DisplayNames
		{
			std::unordered_map<std::string, std::string> maps;
			std::unordered_map<std::string, std::string> gametypes;
			std::unordered_map<std::string, std::string> modes;
		};

		DisplayNames g_display_names;

		std::unordered_map<std::string, std::string> parse_string_map(const rapidjson::Value& root, const char* key)
		{
			std::unordered_map<std::string, std::string> result;

			const auto member = root.FindMember(key);
			if (member == root.MemberEnd() || !member->value.IsObject())
				return result;

			for (auto item = member->value.MemberBegin(); item != member->value.MemberEnd(); ++item)
			{
				if (item->name.IsString() && item->value.IsString())
				{
					result.emplace(
						std::string(item->name.GetString(), item->name.GetStringLength()),
						std::string(item->value.GetString(), item->value.GetStringLength()));
				}
			}

			return result;
		}

		const char* get_mode_key(eModes mode)
		{
			switch (mode)
			{
			case MODE_MULTIPLAYER:
				return "mp";
			case MODE_ZOMBIES:
				return "zm";
			case MODE_CAMPAIGN:
				return "sp";
			default:
				return "none";
			}
		}
	}

	void load_display_names()
	{
		const auto json = utils::nt::load_resource(MAP_MODE_LIST);

		if (json.empty())
			return;

		rapidjson::Document doc{};
		const rapidjson::ParseResult parse_result = doc.Parse(json.data(), json.size());

		if (parse_result.IsError() || !doc.IsObject())
			return;

		g_display_names.maps = parse_string_map(doc, "maps");
		g_display_names.gametypes = parse_string_map(doc, "gametypes");
		g_display_names.modes = parse_string_map(doc, "modes");
	}

	std::string get_map_display_name(const std::string& mapname)
	{
		if (mapname.empty())
			return {};

		const auto it = g_display_names.maps.find(mapname);
		return it != g_display_names.maps.end() ? it->second : mapname;
	}

	std::string get_gametype_display_name(const std::string& gametype)
	{
		if (gametype.empty())
			return {};

		const auto it = g_display_names.gametypes.find(gametype);
		return it != g_display_names.gametypes.end() ? it->second : gametype;
	}

	std::string get_mode_display_name(const std::string& mode_key)
	{
		if (mode_key.empty())
			return {};

		const auto it = g_display_names.modes.find(mode_key);
		return it != g_display_names.modes.end() ? it->second : mode_key;
	}

	std::string get_mode_display_name(eModes mode)
	{
		return get_mode_display_name(get_mode_key(mode));
	}

	std::string get_dvar_string(const char* dvar_name)
	{
		const auto* dvar = Dvar_FindVar(dvar_name);
		if (!dvar)
		{
			return {};
		}

		return Dvar_GetString(dvar);
	}

	int get_dvar_int(const char* dvar_name)
	{
		const auto* dvar = Dvar_FindVar(dvar_name);
		if (!dvar)
		{
			return {};
		}

		return dvar->current.value.integer;
	}

	bool get_dvar_bool(const char* dvar_name)
	{
		const auto* dvar = Dvar_FindVar(dvar_name);
		if (!dvar)
		{
			return {};
		}

		return dvar->current.value.enabled;
	}

	const dvar_t* register_sessionmode_dvar_bool(const char* dvar_name, const bool value, const unsigned int flags,
	                                             const char* description, const eModes mode)
	{
		const auto hash = Dvar_GenerateHash(dvar_name);
		auto* registered_dvar = Dvar_SessionModeRegisterBool(hash, dvar_name, value, flags, description);

		if (registered_dvar)
		{
			registered_dvar->debugName = dvar_name;

			if (mode == MODE_COUNT)
			{
				for (int i = MODE_FIRST; i < MODE_COUNT; ++i)
				{
					game::Dvar_SessionModeSetDefaultBool.call_safe(hash, value, static_cast<eModes>(i));
				}
			}
			else
			{
				game::Dvar_SessionModeSetDefaultBool.call_safe(hash, value, mode);
			}
		}

		return registered_dvar;
	}

	const dvar_t* register_dvar_bool(const char* dvar_name, const bool value, const unsigned int flags, const char* description)
	{
		const auto hash = Dvar_GenerateHash(dvar_name);
		auto* registered_dvar = Dvar_RegisterBool(hash, dvar_name, value, flags, description);

		if (registered_dvar)
		{
			registered_dvar->debugName = dvar_name;
		}

		return registered_dvar;
	}

	const dvar_t* register_dvar_int(const char* dvar_name, int value, int min, int max, const unsigned int flags,
		const char* description)
	{
		const auto hash = Dvar_GenerateHash(dvar_name);
		auto* registered_dvar = Dvar_RegisterInt(hash, dvar_name, value, min, max, flags, description);

		if (registered_dvar)
		{
			registered_dvar->debugName = dvar_name;
		}

		return registered_dvar;
	}

	const dvar_t* register_dvar_float(const char* dvar_name, float value, float min, float max, const unsigned int flags,
	                                  const char* description)
	{
		const auto hash = Dvar_GenerateHash(dvar_name);
		auto* registered_dvar = Dvar_RegisterFloat(hash, dvar_name, value, min, max, flags, description);

		if (registered_dvar)
		{
			registered_dvar->debugName = dvar_name;
		}

		return registered_dvar;
	}

	const dvar_t* register_dvar_string(const char* dvar_name, const char* value, const unsigned int flags,
	                                   const char* description)
	{
		const auto hash = Dvar_GenerateHash(dvar_name);
		auto* registered_dvar = Dvar_RegisterString(hash, dvar_name, value, flags, description);

		if (registered_dvar)
		{
			registered_dvar->debugName = dvar_name;
		}

		return registered_dvar;
	}

	void dvar_add_flags(const char* dvar_name, const unsigned int flags)
	{
		auto* dvar = Dvar_FindVar(dvar_name);

		if (!dvar)
		{
			return;
		}

		auto* dvar_to_change = dvar;
		if (dvar_to_change->type == DVAR_TYPE_SESSIONMODE_BASE_DVAR)
		{
			const auto mode = Com_SessionMode_GetMode();
			dvar_to_change = Dvar_GetSessionModeSpecificDvar(dvar_to_change, static_cast<eModes>(mode));
		}

		dvar_to_change->flags |= flags;
	}

	void dvar_set_flags(const char* dvar_name, const unsigned int flags)
	{
		auto* dvar = Dvar_FindVar(dvar_name);

		if (!dvar)
		{
			return;
		}

		auto* dvar_to_change = dvar;
		if (dvar_to_change->type == DVAR_TYPE_SESSIONMODE_BASE_DVAR)
		{
			const auto mode = Com_SessionMode_GetMode();
			dvar_to_change = Dvar_GetSessionModeSpecificDvar(dvar_to_change, static_cast<eModes>(mode));
		}

		dvar_to_change->flags = flags;
	}

	bool is_server_running()
	{
		return get_dvar_bool("sv_running");
	}

	size_t get_max_client_count()
	{
		return static_cast<size_t>(get_dvar_int("com_maxclients"));
	}

	template <typename T>
	static void foreach_client(T* client_states, const std::function<void(client_s&, size_t index)>& callback)
	{
		if (!client_states || !callback)
		{
			return;
		}

		for (size_t i = 0; i < get_max_client_count(); ++i)
		{
			callback(client_states[i], i);
		}
	}


	template <typename T>
	static bool access_client(T* client_states, const size_t index, const std::function<void(client_s&)>& callback)
	{
		if (!client_states || !callback)
		{
			return false;
		}

		if (index >= get_max_client_count())
		{
			return false;
		}

		auto& client = client_states[index];
		if (client.state == CS_FREE)
		{
			return false;
		}

		callback(client);
		return true;
	}

	void foreach_client(const std::function<void(client_s&, size_t index)>& callback)
	{
		if (is_server())
		{
			foreach_client(*svs_clients, callback);
		}
		else
		{
			foreach_client(*svs_clients_cl, callback);
		}
	}

	void foreach_client(const std::function<void(client_s&)>& callback)
	{
		foreach_client([&](client_s& client, size_t)
		{
			callback(client);
		});
	}

	void foreach_connected_client(const std::function<void(client_s&, size_t index)>& callback)
	{
		foreach_client([&](client_s& client, const size_t index)
		{
			if (client.state != CS_FREE)
			{
				callback(client, index);
			}
		});
	}

	void foreach_connected_client(const std::function<void(client_s&)>& callback)
	{
		foreach_connected_client([&](client_s& client, size_t)
		{
			callback(client);
		});
	}

	bool access_connected_client(const size_t index, const std::function<void(client_s&)>& callback)
	{
		if (is_server())
		{
			return access_client(*svs_clients, index, callback);
		}

		return access_client(*svs_clients_cl, index, callback);
	}
}
