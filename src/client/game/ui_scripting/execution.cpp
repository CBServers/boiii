#include <std_include.hpp>
#include "execution.hpp"

namespace ui_scripting
{
	namespace
	{
		// The HKS api stack is reallocated (and the old buffer freed) whenever it has to grow, so a
		// raw HksObject* cached across a VM call dangles. Save an index relative to bottom instead,
		// exactly like hks_vm_call_internal does internally.
		std::ptrdiff_t save_top(game::hks::lua_State* state)
		{
			return state->m_apistack.top - state->m_apistack.bottom;
		}

		game::hks::HksObject* restore_top(game::hks::lua_State* state, const std::ptrdiff_t index)
		{
			return state->m_apistack.bottom + index;
		}

		script_value get_field(void* ptr, game::hks::HksObjectType type, const script_value& key)
		{
			const auto state = *game::hks::lua_state;
			const auto top_index = save_top(state);

			push_value(key);

			game::hks::HksObject value{};
			game::hks::HksObject obj{};
			obj.t = type;
			obj.v.ptr = ptr;

			game::hks::hks_obj_gettable(&value, state, &obj, &state->m_apistack.top[-1]);
			state->m_apistack.top = restore_top(state, top_index);
			return value;
		}

		void set_field(void* ptr, game::hks::HksObjectType type, const script_value& key, const script_value& value)
		{
			const auto state = *game::hks::lua_state;

			game::hks::HksObject obj{};
			obj.t = type;
			obj.v.ptr = ptr;

			game::hks::hks_obj_settable(state, &obj, &key.get_raw(), &value.get_raw());
		}
	}

	void push_value(const script_value& value)
	{
		const auto state = *game::hks::lua_state;
		*state->m_apistack.top = value.get_raw();
		state->m_apistack.top++;
	}

	void push_value(const game::hks::HksObject& value)
	{
		const auto state = *game::hks::lua_state;
		*state->m_apistack.top = value;
		state->m_apistack.top++;
	}

	script_value get_return_value(std::int64_t offset)
	{
		const auto state = *game::hks::lua_state;
		return state->m_apistack.top[-1 - offset];
	}

	arguments get_return_values()
	{
		const auto state = *game::hks::lua_state;
		const auto count = state->m_apistack.top - state->m_apistack.base;
		arguments values;

		for (auto i = count - 1; i >= 0; i--)
		{
			values.push_back(get_return_value(i));
		}

		if (values.empty())
		{
			values.push_back({});
		}

		return values;
	}

	arguments get_return_values(game::hks::HksObject* base)
	{
		const auto state = *game::hks::lua_state;
		const auto count = state->m_apistack.top - base;
		arguments values;

		for (auto i = count - 1; i >= 0; i--)
		{
			values.push_back(get_return_value(i));
		}

		if (values.empty())
		{
			values.push_back({});
		}

		return values;
	}

	bool notify(const std::string& name, const event_arguments& arguments)
	{
		const auto state = *game::hks::lua_state;
		if (state == nullptr)
		{
			return false;
		}

		//const auto _0 = gsl::finally(game::LUI_LeaveCriticalSection);
		//game::LUI_EnterCriticalSection();

		try
		{
			const auto globals = table((*::game::hks::lua_state)->globals.v.table);
			const auto engine = globals.get("Engine").as<table>();
			const auto root = engine.get("GetLuiRoot")()[0].as<userdata>();
			const auto process_event = root.get("processEvent");

			table event{};
			event.set("name", name);
			event.set("dispatchChildren", true);

			for (const auto& arg : arguments)
			{
				event.set(arg.first, arg.second);
			}

			process_event(root, event);
			return true;
		}
		catch (const std::exception& ex)
		{
			printf("Error processing event '%s' %s\n", name.data(), ex.what());
		}

		return false;
	}

	arguments call_script_function(const function& function, const arguments& arguments)
	{
		const auto state = *game::hks::lua_state;
		const auto top_index = save_top(state);

		push_value(function);
		for (auto i = arguments.begin(); i != arguments.end(); ++i)
		{
			push_value(*i);
		}

		game::hks::vm_call_internal(state, static_cast<int>(arguments.size()), -1, nullptr);

		const auto top = restore_top(state, top_index);
		const auto args = get_return_values(top);
		state->m_apistack.top = top;
		return args;
	}

	script_value get_field(const userdata& self, const script_value& key)
	{
		return get_field(self.ptr, game::hks::TUSERDATA, key);
	}

	script_value get_field(const table& self, const script_value& key)
	{
		return get_field(self.ptr, game::hks::TTABLE, key);
	}

	void set_field(const userdata& self, const script_value& key, const script_value& value)
	{
		set_field(self.ptr, game::hks::TUSERDATA, key, value);
	}

	void set_field(const table& self, const script_value& key, const script_value& value)
	{
		set_field(self.ptr, game::hks::TTABLE, key, value);
	}
}
