-- Prompts for a password (via the in-game keyboard) when joining a password-protected server.
if Engine.GetCurrentMap() ~= "core_frontend" then
	return
end

local pendingJoin = nil

local function GetServerModelValue(model, key)
	local keyModel = Engine.GetModel(model, key)
	if keyModel then
		return Engine.GetModelValue(keyModel)
	end
	return nil
end

local function ConnectToServer(controller, serverIndex, name, connectAddr)
	if serverIndex and name then
		Engine.SteamServerBrowser_AddFavoriteServer(serverIndex, true)
	end
	if connectAddr then
		Engine.Exec(controller, "connect " .. connectAddr)
	end
end

local function EnsureKeyboardHandler(menu)
	if not menu or menu.serverPasswordKeyboardHandler then
		return
	end
	menu.serverPasswordKeyboardHandler = true

	local previousHandler = menu.m_eventHandlers and menu.m_eventHandlers["ui_keyboard_input"]
	menu:registerEventHandler("ui_keyboard_input", function(element, event)
		if event.type == Enum.KeyboardType.KEYBOARD_TYPE_SERVER_PASSWORD and pendingJoin then
			local join = pendingJoin
			pendingJoin = nil
			Engine.SetDvar("password", event.input or "")
			ConnectToServer(join.controller, join.serverIndex, join.name, join.connectAddr)
			GoBack(join.widget, join.controller)
			return true
		end
		if previousHandler then
			return previousHandler(element, event)
		end
		return false
	end)
end

JoinServerBrowser = function(widget, element, controller, menu)
	local model = element:getModel()
	if not model then
		return
	end

	local serverIndex = GetServerModelValue(model, "serverIndex")
	local name = GetServerModelValue(model, "name")
	local connectAddr = GetServerModelValue(model, "connectAddr")

	if not GetServerModelValue(model, "passwordProtected") then
		ConnectToServer(controller, serverIndex, name, connectAddr)
		GoBack(widget, controller)
		return
	end

	pendingJoin = {
		widget = widget,
		controller = controller,
		serverIndex = serverIndex,
		name = name,
		connectAddr = connectAddr
	}

	EnsureKeyboardHandler(menu or widget)
	ShowKeyboard(widget, element, controller, "KEYBOARD_TYPE_SERVER_PASSWORD")
end
