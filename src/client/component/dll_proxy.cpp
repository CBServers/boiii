#include <std_include.hpp>
#include "dll_proxy.hpp"
#include "scheduler.hpp"
#include "loader/component_loader.hpp"

#include <utils/flags.hpp>
#include <utils/io.hpp>
#include <utils/nt.hpp>
#include <utils/string.hpp>

namespace dll_proxy
{
	namespace
	{
		struct proxy_dll
		{
			std::string name;
			bool blocked;
		};

		// Shipped by the game, so they always load from the game folder even if a system copy exists
		const std::unordered_set<std::wstring> game_dlls
		{
			L"steam_api64.dll",
		};

		std::vector<proxy_dll>& get_proxies()
		{
			static std::vector<proxy_dll> proxies{};
			return proxies;
		}

		std::filesystem::path get_system_folder()
		{
			std::wstring buffer{};
			buffer.resize(MAX_PATH);

			const auto length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
			if (!length || length >= buffer.size())
			{
				return {};
			}

			buffer.resize(length);
			return buffer;
		}

		std::wstring get_lowercase_filename(const std::filesystem::path& file)
		{
			auto filename = file.filename().wstring();
			std::ranges::transform(filename, filename.begin(), ::towlower);
			return filename;
		}

		bool is_same_file(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			std::error_code code{};
			return std::filesystem::equivalent(a, b, code);
		}
	}

	// A dll in the game folder that shares a name with a system dll and isn't shipped by the game is a
	// proxy injecting itself into the process. Claiming the name with the system copy keeps it out.
	void block()
	{
		if (utils::flags::has_flag("allowproxydlls"))
		{
			return;
		}

		const auto system_folder = get_system_folder();
		if (system_folder.empty())
		{
			return;
		}

		const auto self = utils::nt::library::get_by_address(&block);

		for (const auto& file : utils::io::list_files(self.get_folder()))
		{
			const auto filename = get_lowercase_filename(file);
			if (!filename.ends_with(L".dll") || game_dlls.contains(filename))
			{
				continue;
			}

			const auto system_file = system_folder / filename;
			if (!utils::io::file_exists(system_file.wstring()))
			{
				continue;
			}

			const utils::nt::library loaded{LoadLibraryW(system_file.wstring().data())};
			if (!loaded)
			{
				continue;
			}

			// A proxy named after one of our own imports is already loaded by the time we get here
			const auto blocked = is_same_file(loaded.get_path(), system_file);
			get_proxies().emplace_back(utils::string::convert(filename), blocked);
		}
	}

	struct component final : generic_component
	{
		void post_unpack() override
		{
			if (get_proxies().empty())
			{
				return;
			}

			scheduler::once([]
			{
				for (const auto& proxy : get_proxies())
				{
					if (proxy.blocked)
					{
						printf("Ignored proxy dll '%s' in the game folder, loaded the system one instead\n",
						       proxy.name.data());
					}
					else
					{
						printf("Proxy dll '%s' was loaded before it could be blocked\n", proxy.name.data());
					}
				}

				printf("Launch with -allowproxydlls to load proxy dlls from the game folder\n");
			}, scheduler::pipeline::main);
		}
	};
}

REGISTER_COMPONENT(dll_proxy::component)
