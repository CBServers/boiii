#include <std_include.hpp>

#include "updater.hpp"
#include "updater_ui.hpp"
#include "file_updater.hpp"

#include <utils/cryptography.hpp>
#include <utils/flags.hpp>
#include <utils/http.hpp>
#include <utils/io.hpp>
#include <utils/compression.hpp>
#include <utils/properties.hpp>

#define UPDATE_SERVER "https://github.com/CBServers/updater/raw/main/updater/"
#define UPDATE_FILE_MAIN UPDATE_SERVER "boiii.json"
#define UPDATE_FOLDER_MAIN UPDATE_SERVER "boiii/"
#define UPDATE_HOST_BINARY "boiii.exe"

#define MIGRATE_TO_T7X
#define T7X_UPDATE_SERVER "https://master.bo3.eu/"
#define T7X_UPDATE_FILE_MAIN  T7X_UPDATE_SERVER "files.json"
#define T7X_UPDATE_FOLDER_MAIN  T7X_UPDATE_SERVER "t7x/"
#define T7X_HOST_BINARY "t7x.exe"

#define CACHE_FOLDER game::get_appdata_cache_path()

namespace updater
{
	namespace
	{
		std::filesystem::path get_cache_folder()
		{
			return CACHE_FOLDER;
		}

		std::vector<file_info> parse_file_infos(const std::string& json)
		{
			rapidjson::Document doc{};
			doc.Parse(json.data(), json.size());

			if (!doc.IsArray())
			{
				return {};
			}

			std::vector<file_info> files{};

			for (const auto& element : doc.GetArray())
			{
				if (!element.IsArray())
				{
					continue;
				}

				auto array = element.GetArray();

				file_info info{};
				info.name.assign(array[0].GetString(), array[0].GetStringLength());
				info.size = array[1].GetInt64();
				info.hash.assign(array[2].GetString(), array[2].GetStringLength());

				files.emplace_back(std::move(info));
			}

			return files;
		}

		std::vector<file_info> parse_t7x_file_infos(const std::string& json)
		{
			rapidjson::Document doc{};
			doc.Parse(json.data(), json.size());

			if (!doc.IsArray())
			{
				return {};
			}

			std::vector<file_info> files{};

			for (const auto& element : doc.GetArray())
			{
				if (!element.IsObject())
				{
					continue;
				}

				file_info info{};

				if (element.HasMember("name") && element["name"].IsString())
				{
					info.name.assign(element["name"].GetString(), element["name"].GetStringLength());
				}

				if (element.HasMember("size") && element["size"].IsInt64())
				{
					info.size = element["size"].GetInt64();
				}

				if (element.HasMember("hash") && element["hash"].IsString())
				{
					info.hash.assign(element["hash"].GetString(), element["hash"].GetStringLength());
				}

				files.emplace_back(std::move(info));
			}

			return files;
		}

		std::string get_cache_buster()
		{
			return "?" + std::to_string(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
		}

		std::vector<file_info> get_file_infos(const std::string& url)
		{
			const auto json = utils::http::get_data(url + get_cache_buster());
			if (!json)
			{
				return {};
			}

			if (url == T7X_UPDATE_FILE_MAIN)
			{
				return parse_t7x_file_infos(*json);
			}

			return parse_file_infos(*json);
		}

		std::vector<std::string> get_cache_files()
		{
			std::vector<std::string> cache_files =
			{
				{"cache.bin"},
				{"data.bin"},
			};

			return cache_files;
		}

		bool has_cache_file(const std::string& file)
		{
			const auto path = get_cache_folder() / file;
			return utils::io::file_exists(path);
		}

		std::vector<std::string> check_cache_files()
		{
			const auto files = get_cache_files();
			std::vector<std::string> cache_files{};
			
			for (const auto& file : files)
			{
				if (!has_cache_file(file))
				{
					cache_files.emplace_back(file);
				}
			}

			return cache_files;
		}

		void create_cache_files(const std::vector<std::string>& files)
		{
			const auto cache_folder = get_cache_folder();
			if (!utils::io::directory_exists(cache_folder))
			{
				utils::io::create_directory(cache_folder);
			}

			std::string data;
			for (const auto& file : files)
			{
				if (!utils::io::write_file(cache_folder / file, data))
				{
					throw std::runtime_error("Failed to write: " + file);
				}
			}
		}

		std::string get_hash(const std::string& data)
		{
			return utils::cryptography::sha1::compute(data, true);
		}

		const file_info* find_host_file_info(const std::vector<file_info>& outdated_files, const std::string& host_binary)
		{
			for (const auto& file : outdated_files)
			{
				if (file.name == host_binary)
				{
					return &file;
				}
			}

			return nullptr;
		}

		size_t get_optimal_concurrent_download_count(const size_t file_count)
		{
			size_t cores = std::thread::hardware_concurrency();
			cores = (cores * 2) / 3;
			return std::max(1ull, std::min(cores, file_count));
		}

		bool is_inside_folder(const std::filesystem::path& file, const std::filesystem::path& folder)
		{
			const auto relative = std::filesystem::relative(file, folder);
			const auto start = relative.begin();
			return start != relative.end() && start->string() != "..";
		}
	}

	file_updater::file_updater(progress_listener& listener, std::filesystem::path base,
		std::filesystem::path game_base, std::filesystem::path process_file)
		: listener_(listener)
		  , base_(std::move(base))
		  , game_base_(std::move(game_base))
		  , process_file_(std::move(process_file))
		  , dead_process_file_(process_file_)
	{
		this->dead_process_file_.replace_extension(".exe.old");
		this->delete_old_process_file();
	}

	void file_updater::run() const
	{
		const auto files = get_file_infos(UPDATE_FILE_MAIN);
		if (files.empty())
		{
			return;
		}

		this->update_host_binary(files);
#ifndef MIGRATE_TO_T7X
		this->cleanup_directories(files);

		const auto outdated_files = this->get_outdated_files(files);
		if (outdated_files.empty())
		{
			return;
		}

		this->update_files(outdated_files, UPDATE_FOLDER_MAIN);

		const auto missing_cache_files = check_cache_files();
		if (!missing_cache_files.empty())
		{
			create_cache_files(missing_cache_files);
		}
#else
		this->migrate_players_to_t7x();
		
		const auto t7x_files = get_file_infos(T7X_UPDATE_FILE_MAIN);
		if (t7x_files.empty())
		{
			return;
		}

		this->update_and_launch_t7x(t7x_files);

		std::this_thread::sleep_for(2s);
		this->migrate_keys_to_t7x();

		throw update_cancelled();
#endif
	}

	void file_updater::update_file(const file_info& file, const std::string& file_url) const
	{
		const auto url = file_url + file.name + "?" + file.hash;

		const auto data = utils::http::get_data(url, {}, [&](const size_t progress)
		{
			this->listener_.file_progress(file, progress);
		});

		if (!data || (data->size() != file.size || get_hash(*data) != file.hash))
		{
			throw std::runtime_error("Failed to download: " + url);
		}

		const auto out_file = this->get_drive_filename(file);
		if (!utils::io::write_file(out_file, *data, false))
		{
			throw std::runtime_error("Failed to write: " + file.name);
		}
	}

	std::vector<file_info> file_updater::get_outdated_files(const std::vector<file_info>& files) const
	{
		std::vector<file_info> outdated_files{};

		for (const auto& info : files)
		{
			if (this->is_outdated_file(info))
			{
				outdated_files.emplace_back(info);
			}
		}

		return outdated_files;
	}

	void file_updater::update_host_binary(const std::vector<file_info>& outdated_files) const
	{
		const auto* host_file = find_host_file_info(outdated_files, UPDATE_HOST_BINARY);
		if (!host_file)
		{
			return;
		}

		if (!this->is_outdated_file(*host_file))
		{
			return;
		}

		try
		{
			this->move_current_process_file();
			this->update_files({*host_file}, UPDATE_FOLDER_MAIN);
		}
		catch (...)
		{
			this->restore_current_process_file();
			throw;
		}

		if (!utils::flags::has_flag("norelaunch"))
		{
			utils::nt::relaunch_self();
		}

		throw update_cancelled();
	}

	void file_updater::update_and_launch_t7x(const std::vector<file_info>& files) const
	{
		const auto* host_file = find_host_file_info(files, T7X_HOST_BINARY);
		if (!host_file)
		{
			return;
		}

		if (this->is_outdated_file(*host_file))
		{
			try
			{
				this->update_files({*host_file}, T7X_UPDATE_FOLDER_MAIN);
			}
			catch (...)
			{
				throw;
			}
		}

		const auto t7x = this->game_base_ / T7X_HOST_BINARY;
		if (utils::io::file_exists(t7x))
		{
			utils::nt::launch_process(t7x);
		}
	}

	void file_updater::update_files(const std::vector<file_info>& outdated_files, const std::string& url) const
	{
		this->listener_.update_files(outdated_files);

		const auto thread_count = get_optimal_concurrent_download_count(outdated_files.size());

		std::vector<std::thread> threads{};
		std::atomic<size_t> current_index{0};

		utils::concurrency::container<std::exception_ptr> exception{};

		for (size_t i = 0; i < thread_count; ++i)
		{
			threads.emplace_back([&]()
			{
				while (!exception.access<bool>([](const std::exception_ptr& ptr)
				{
					return static_cast<bool>(ptr);
				}))
				{
					const auto index = current_index++;
					if (index >= outdated_files.size())
					{
						break;
					}

					try
					{
						const auto& file = outdated_files[index];
						this->listener_.begin_file(file);
						this->update_file(file, url);
						this->listener_.end_file(file);
					}
					catch (...)
					{
						exception.access([](std::exception_ptr& ptr)
						{
							ptr = std::current_exception();
						});

						return;
					}
				}
			});
		}

		for (auto& thread : threads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		exception.access([](const std::exception_ptr& ptr)
		{
			if (ptr)
			{
				std::rethrow_exception(ptr);
			}
		});

		this->listener_.done_update();
	}

	bool file_updater::is_outdated_file(const file_info& file) const
	{
#if !defined(NDEBUG)
		if (file.name == UPDATE_HOST_BINARY && !utils::flags::has_flag("update"))
		{
			return false;
		}
#endif

		std::string data{};
		const auto drive_name = this->get_drive_filename(file);
		if (!utils::io::read_file(drive_name, &data))
		{
			return true;
		}

		if (data.size() != file.size)
		{
			return true;
		}

		const auto hash = get_hash(data);
		return hash != file.hash;
	}

	std::filesystem::path file_updater::get_drive_filename(const file_info& file) const
	{
		if (file.name == UPDATE_HOST_BINARY)
		{
			return this->process_file_;
		}

		if (file.name == T7X_HOST_BINARY)
		{
			return this->game_base_ / file.name;
		}

		return this->base_ / file.name;
	}

	void file_updater::move_current_process_file() const
	{
		utils::io::move_file(this->process_file_, this->dead_process_file_);
	}

	void file_updater::restore_current_process_file() const
	{
		utils::io::move_file(this->dead_process_file_, this->process_file_);
	}

	void file_updater::delete_old_process_file() const
	{
		// Wait for other process to die
		for (auto i = 0; i < 4; ++i)
		{
			utils::io::remove_file(this->dead_process_file_);
			if (!utils::io::file_exists(this->dead_process_file_))
			{
				break;
			}

			std::this_thread::sleep_for(2s);
		}
	}

	void file_updater::cleanup_directories(const std::vector<file_info>& files) const
	{
		if (!utils::io::directory_exists(this->base_))
		{
			return;
		}

		this->cleanup_root_directory(files);
		this->cleanup_data_directory(files);
	}

	void file_updater::cleanup_root_directory(const std::vector<file_info>& files) const
	{
		const auto existing_files = utils::io::list_files(this->base_);
		for (const auto& file : existing_files)
		{
			const auto entry = std::filesystem::relative(file, this->base_);
			if ((entry.string() == "user" || entry.string() == "data") && utils::io::directory_exists(file))
			{
				continue;
			}

			bool found = false;
			for (const auto& wantedFile : files)
			{
				if (wantedFile.name == entry)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				std::error_code code{};
				std::filesystem::remove_all(file, code);
			}
		}
	}

	void file_updater::cleanup_data_directory(const std::vector<file_info>& files) const
	{
		const auto base = std::filesystem::path(this->base_);
		if (!utils::io::directory_exists(base.string()))
		{
			return;
		}

		std::vector<std::filesystem::path> legal_files{};
		legal_files.reserve(files.size());
		for (const auto& file : files)
		{
			if (file.name.starts_with("data"))
			{
				legal_files.emplace_back(std::filesystem::absolute(base / file.name));
			}
		}

		const auto existing_files = utils::io::list_files(base / "data", true);
		for (auto& file : existing_files)
		{
			const auto is_file = std::filesystem::is_regular_file(file);
			const auto is_folder = std::filesystem::is_directory(file);

			if (is_file || is_folder)
			{
				bool is_legal = false;

				for (const auto& legal_file : legal_files)
				{
					if ((is_folder && is_inside_folder(legal_file, file)) ||
						(is_file && legal_file == file))
					{
						is_legal = true;
						break;
					}
				}

				if (is_legal)
				{
					continue;
				}
			}

			std::error_code code{};
			std::filesystem::remove_all(file, code);
		}
	}

	void file_updater::migrate_players_to_t7x() const
	{
		const auto boiii_players = this->game_base_ / "boiii_players";
		const auto t7x_players = this->game_base_ / "t7x" / "players";
		const auto migrated_file = t7x_players / "migrated_from_boiii";

		if (!utils::io::file_exists(migrated_file))
		{
			if (utils::io::directory_exists(boiii_players))
			{
				if (utils::io::directory_exists(t7x_players))
				{
					utils::io::remove_folder(t7x_players);
				}

				utils::io::create_directory(t7x_players);
				utils::io::copy_folder(boiii_players, t7x_players);
			}

			std::string data{};
			utils::io::write_file(migrated_file, data);
		}
	}

	void file_updater::migrate_keys_to_t7x() const
	{
		const auto t7x_folder = utils::properties::get_appdata_path("t7x");

		while (!utils::io::directory_exists(t7x_folder))
		{
			std::this_thread::sleep_for(1s); //Wait untill t7x creates the folder
		}

		const auto appdata_path = utils::properties::get_key_path();

		const std::map<std::string, std::string> key_mappings = {
			{ "cb-private.key", "t7x-private.key" },
			{ "cb-public.key", "t7x-public.key" }
		};

		for (const auto& [source_name, dest_name] : key_mappings)
		{
			const auto source_file = appdata_path / source_name;
			if (utils::io::file_exists(source_file))
			{
				std::string data{};
				if (utils::io::read_file(source_file, &data))
				{
					utils::io::write_file(t7x_folder / dest_name, dest_name);
				}
			}
		}
	}
}
