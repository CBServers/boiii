-- Zombies' HUD never instantiates the toast widget Multiplayer's does, so ShowToast is a no-op in ZM; add it ourselves.
if not CoD.isZombie then
	return
end

local function ensureToast(hud)
	if hud == nil or hud.toastNotification ~= nil or CoD.ToastNotification == nil then
		return
	end
	local controller = hud.controller or Engine.GetPrimaryController()
	local toast = CoD.ToastNotification.new(hud, controller)
	toast:setState("DefaultState")
	toast:setPriority(9999)
	hud.toastNotification = toast
	local parent = hud:getParent()
	if parent then
		parent:addElement(toast)
	end
end

-- Cover future HUD (re)builds, mirroring HUD_FirstSnapshot_Multiplayer.
if HUD_FirstSnapshot_Zombie ~= nil then
	local oldZombieFirstSnapshot = HUD_FirstSnapshot_Zombie
	HUD_FirstSnapshot_Zombie = function(hud, event)
		oldZombieFirstSnapshot(hud, event)
		ensureToast(hud)
	end
end

-- Our scripts load just after the HUD is first built, so also patch the already-live HUD.
if LUI ~= nil and LUI.roots ~= nil then
	local huds = {}
	for _, root in pairs(LUI.roots) do
		if type(root) == "table" and root.getFirstChild ~= nil then
			local child = root:getFirstChild()
			while child ~= nil do
				if child.id == "Menu.HUD" then
					huds[#huds + 1] = child
				end
				child = child:getNextSibling()
			end
		end
	end
	for _, hud in ipairs(huds) do
		ensureToast(hud)
	end
end
