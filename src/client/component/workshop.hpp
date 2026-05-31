#pragma once

#include <string>
#include <vector>

#include <game/game.hpp>

namespace workshop
{
	extern std::vector<game::workshop_data> custom_workshop_pool;

	std::string get_usermap_publisher_id(const std::string& folder_name, const bool& get_info = false);
	std::string get_usermap_path(const std::string& mapname, const std::string& pub_id);
	std::string get_mod_publisher_id();
	std::string get_mod_resized_name();
	bool check_valid_usermap_id(const std::string& mapname, const std::string& pub_id, const std::string& base_url);
	bool check_valid_mod_id(const std::string& pub_id);
	void setup_same_mod_as_host(const std::string& usermap, const std::string& mod);
}
