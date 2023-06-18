#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include <MinHook.h>
#include <Windows.h>

#include <utils/nt.hpp>
#include <game/game.hpp>

namespace extension
{
	typedef BOOL(WINAPI* PTERMINATE_PROCESS)(HANDLE hProcess, UINT uExitCode);
	typedef VOID(WINAPI* PEXIT_PROCESS)(UINT uExitCode);

	PTERMINATE_PROCESS g_pTerminateProcess = nullptr;
	PEXIT_PROCESS g_pExitProcess = nullptr;

	bool IsAddressInModule(HMODULE hModule, void* address)
	{
		if (!hModule || !address)
		{
			return false;
		}

		MODULEINFO moduleInfo;
		if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)))
		{
			return false;
		}

		uintptr_t moduleStart = reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll);
		uintptr_t moduleEnd = moduleStart + moduleInfo.SizeOfImage;

		return reinterpret_cast<uintptr_t>(address) >= moduleStart && reinterpret_cast<uintptr_t>(address) <= moduleEnd;
	}

	PTERMINATE_PROCESS pOrigTerminateProcess = nullptr;
	BOOL WINAPI terminate_process_stub(HANDLE hProcess, UINT uExitCode)
	{
		PVOID caller = _ReturnAddress();
		HMODULE hExtDll = GetModuleHandle(TEXT("ext.dll"));

		if (IsAddressInModule(hExtDll, caller))
		{
			// Block TerminateProcess for ext.dll
			return TRUE;
		}
		else
		{
			// Call original function for other modules
			return pOrigTerminateProcess(hProcess, uExitCode);
		}
	}

	PEXIT_PROCESS pOrigExitProcess = nullptr;
	VOID WINAPI exit_process_stub(UINT uExitCode)
	{
		PVOID caller = _ReturnAddress();
		HMODULE hExtDll = GetModuleHandle(TEXT("ext.dll"));

		if (!IsAddressInModule(hExtDll, caller))
		{
			// Call original function for other modules
			pOrigExitProcess(uExitCode);
		}
		// Block ExitProcess for ext.dll
	}

	struct component final : generic_component
	{
		component()
		{
			this->extension_ = utils::nt::library::load(game::get_appdata_path() / "ext.dll");

			auto g_pTerminateProcess = (PTERMINATE_PROCESS)GetProcAddress(GetModuleHandle("kernel32"), "TerminateProcess");
			auto g_pExitProcess = (PEXIT_PROCESS)GetProcAddress(GetModuleHandle("kernel32"), "ExitProcess");

			MH_Initialize();

			try
			{
				MH_CreateHook(g_pTerminateProcess, terminate_process_stub, reinterpret_cast<LPVOID*>(&pOrigTerminateProcess));
				MH_EnableHook(reinterpret_cast<LPVOID>(g_pTerminateProcess));

				MH_CreateHook(g_pExitProcess, exit_process_stub, reinterpret_cast<LPVOID*>(&pOrigExitProcess));
				MH_EnableHook(reinterpret_cast<LPVOID>(g_pExitProcess));
			}
			catch (...)
			{
				throw std::runtime_error("Failed to hook ext.dll");
			}
		}

		~component() override
		{
			MH_DisableHook(reinterpret_cast<LPVOID>(g_pTerminateProcess));
			MH_RemoveHook(reinterpret_cast<LPVOID>(g_pTerminateProcess));

			MH_DisableHook(reinterpret_cast<LPVOID>(g_pExitProcess));
			MH_RemoveHook(reinterpret_cast<LPVOID>(g_pExitProcess));

			MH_Uninitialize();

			this->extension_.free();
		}

		void post_load() override
		{
			this->extension_.invoke<void>("_1");
		}

		void post_unpack() override
		{
			this->extension_.invoke<void>("_2");
		}

		void pre_destroy() override
		{
			this->extension_.invoke<void>("_3");
		}

		utils::nt::library extension_{};
	};
}

REGISTER_COMPONENT(extension::component)
