#include "flags.hpp"
#include "string.hpp"
#include "nt.hpp"

#include <shellapi.h>
#include <unordered_map>
#include <unordered_set>

#include "finally.hpp"

namespace utils::flags
{
	std::unordered_map<std::string, std::string> parse_flag_values()
	{
		int num_args{};
		auto* const argv = CommandLineToArgvW(GetCommandLineW(), &num_args);
		const auto _ = finally([&argv]
		{
			if (argv)
			{
				LocalFree(argv);
			}
		});

		std::unordered_map<std::string, std::string> values{};

		for (auto i = 0; argv && i + 1 < num_args; ++i)
		{
			std::wstring wide_flag(argv[i]);
			if (wide_flag.empty() || wide_flag[0] != L'-')
			{
				continue;
			}

			wide_flag.erase(wide_flag.begin());
			auto key = string::to_lower(string::convert(wide_flag));
			values.emplace(std::move(key), string::convert(std::wstring(argv[i + 1])));
		}

		return values;
	}

	std::unordered_set<std::string> parse_flags()
	{
		int num_args{};
		auto* const argv = CommandLineToArgvW(GetCommandLineW(), &num_args);
		const auto _ = finally([&argv]
		{
			if (argv)
			{
				LocalFree(argv);
			}
		});

		std::unordered_set<std::string> flags{};

		for (auto i = 0; i < num_args && argv; ++i)
		{
			std::wstring wide_flag(argv[i]);
			if (wide_flag[0] == L'-')
			{
				wide_flag.erase(wide_flag.begin());
				flags.emplace(string::to_lower(string::convert(wide_flag)));
			}
		}

		return flags;
	}

	bool has_flag(const std::string& flag)
	{
		static const auto enabled_flags = parse_flags();
		return enabled_flags.contains(string::to_lower(flag));
	}

	std::optional<std::string> get_flag_value(const std::string& flag)
	{
		static const auto flag_values = parse_flag_values();
		const auto it = flag_values.find(string::to_lower(flag));
		if (it == flag_values.end())
		{
			return std::nullopt;
		}
		return it->second;
	}
}
