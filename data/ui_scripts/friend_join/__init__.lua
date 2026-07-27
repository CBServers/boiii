-- Reroutes the game's native Social JOIN GAME/JOIN PARTY action (SocialJoin) to our own
-- connect logic for CB friends. The native action resolves the friend's session through real
-- Demonware matchmaking, which our launcher-IPC friends don't have, so game.connectToFriend
-- must drive the join instead. The native button's gating/styling/localization is reused as-is.

-- Register the game's own map/gametype id tables into C++ so launcher presence (raw names)
-- resolves to the native ints the friends rich-presence spoof needs. Runs every UI load; idempotent.
pcall( function ()
	if not ( game and game.registerFriendMap and game.registerFriendGametype ) then
		return
	end

	if CoD and CoD.mapsTable then
		for name, m in pairs( CoD.mapsTable ) do
			if type( m ) == "table" and m.unique_id then
				game.registerFriendMap( name, tostring( m.unique_id ) )
			end
		end
	end

	-- game_types has no Lua enumerator; scan ids (the display code does id-keyed lookups the same way)
	for i = 0, 63 do
		local name = Engine.StructTableLookupString( "game_types", "id", tostring( i ), "name" )
		if name and name ~= "" then
			game.registerFriendGametype( name, tostring( i ) )
		end
	end
end )

pcall( function ()
	-- actions.lua defines the SocialJoin global; force-load in case we run before it
	require( "ui.uieditor.actions" )

	if cbSocialJoinHooked then
		return
	end

	local orig_SocialJoin = SocialJoin
	if not orig_SocialJoin then
		return
	end
	cbSocialJoinHooked = true

	-- signature matches ProcessListAction: (menu, element, controller, actionParam, parentMenu)
	SocialJoin = function ( menu, element, controller, param, parentMenu )
		local xuid = param and param.xuid
		local hex = xuid and Engine.UInt64ToString( xuid )
		if hex and game and game.isFriendJoinable and game.isFriendJoinable( hex ) then
			if game.connectToFriend then
				game.connectToFriend( hex )
			end
			GoBackToMenu( GoBack( menu, controller ), controller, "Lobby" )
			return
		end
		return orig_SocialJoin( menu, element, controller, param, parentMenu )
	end
end )

-- the engine re-reads ISteamFriends only on Engine.UpdateFriends (Social tab open), so while
-- the menu is open, poke it whenever the launcher pushes a new friends snapshot
pcall( function ()
	require( "ui.uieditor.menus.Social.Social_Main" )

	if cbSocialRefreshHooked then
		return
	end

	local orig_createSocialMain = LUI.createMenu.Social_Main
	if not ( orig_createSocialMain and game and game.friendsSnapshotVersion ) then
		return
	end
	cbSocialRefreshHooked = true

	LUI.createMenu.Social_Main = function ( controller )
		local menu = orig_createSocialMain( controller )
		local lastVersion = game.friendsSnapshotVersion()
		menu:addElement( LUI.UITimer.newElementTimer( 2000, false, function ()
			local version = game.friendsSnapshotVersion()
			if version ~= lastVersion then
				lastVersion = version
				Engine.UpdateFriends( controller, Enum.PresenceFilter.PRESENCE_FILTER_ONLINE_AND_IN_TITLE )
			end
		end ) )
		return menu
	end
end )

-- INVITE GAME gating. CoD.canInviteToGame demands a real lobby session, which a member who
-- direct-connected into someone's match never has, so only the host ever saw the button; and it
-- suppresses "already in my lobby" via LobbyIsPlayerInLobby, which cannot see our synthetic
-- friends, so it happily offered to invite someone sitting in the match. C++ answers both, on the
-- same rule as the launcher UI. The send is rerouted too: Engine.SendInviteByXuid wants the same
-- lobby session, so CB friends go straight to friends::request_invite.
pcall( function ()
	if cbInviteGateHooked then
		return
	end

	local orig_canInviteToGame = CoD.canInviteToGame
	local orig_invitePlayer = CoD.invitePlayer
	if not ( orig_canInviteToGame and orig_invitePlayer ) then
		return
	end
	cbInviteGateHooked = true

	CoD.canInviteToGame = function ( controller, xuid )
		local hex = xuid and Engine.UInt64ToString( xuid )
		if hex and game then
			if game.isFriendInMatch and game.isFriendInMatch( hex ) then
				return false
			end
			if game.canInviteFriend and game.canInviteFriend( hex ) then
				return true
			end
		end
		return orig_canInviteToGame( controller, xuid )
	end

	CoD.invitePlayer = function ( controller, xuid, extra )
		local hex = xuid and Engine.UInt64ToString( xuid )
		if hex and game and game.inviteFriend and game.inviteFriend( hex ) then
			return
		end
		return orig_invitePlayer( controller, xuid, extra )
	end
end )

-- a same-match friend is reported as LobbyJoinable 9 (spare slot) rather than plain not-joinable
pcall( function ()
	require( "ui.T6.lobby.presence" )

	if CoD and CoD.Presence then
		if CoD.Presence.LobbyFriendJoinableStrings then
			CoD.Presence.LobbyFriendJoinableStrings[9] = "In your match"
		end
		if CoD.Presence.LobbyRecentPlayersJoinableStrings then
			CoD.Presence.LobbyRecentPlayersJoinableStrings[9] = "In your match"
		end
	end
end )

-- How long a toast dwells is not a ShowToast argument: it is a 2809ms keyframe baked into the
-- ToastNotification "Show" clip. Stretch that one keyframe on the container, for invite toasts only
-- (both the incoming-invite one and INVITE SENT run through state "Invite"). The clip closures call
-- the method on the container instance, so the override has to sit on the instance, not the class.
pcall( function ()
	local DEFAULT_HOLD_MS = 2809
	local INVITE_HOLD_MS = 8000

	local function patchContainer( toast )
		local container = toast and toast.ToastContainer
		if not container then
			return
		end

		-- re-wrap the saved original on reload, so tuning the hold does not need a UI teardown
		local orig_beginAnimation = container.cbHoldOrig or container.beginAnimation
		if type( orig_beginAnimation ) ~= "function" then
			return
		end
		container.cbHoldOrig = orig_beginAnimation

		container.beginAnimation = function ( self, name, duration, ... )
			if duration == DEFAULT_HOLD_MS and self.currentNotification and
				self.currentNotification.state == "Invite" then
				duration = INVITE_HOLD_MS
			end
			return orig_beginAnimation( self, name, duration, ... )
		end
	end

	-- future widgets (ZM builds one in party/zombie_toast.lua, MP/frontend build their own). The
	-- wrapper calls through a global so a luiReload swaps in the reloaded patch, not the captured one.
	cbToastPatchContainer = patchContainer

	if CoD and CoD.ToastNotification and CoD.ToastNotification.new and not cbToastHoldHooked then
		local orig_new = CoD.ToastNotification.new
		cbToastHoldHooked = true

		CoD.ToastNotification.new = function ( menu, controller )
			local toast = orig_new( menu, controller )
			pcall( function () cbToastPatchContainer( toast ) end )
			return toast
		end
	end

	-- and the one the HUD/frontend menu already built before our scripts loaded. Elements are
	-- userdata and their accessors are native, so go through pcall rather than field tests.
	local function try( element, method )
		local ok, result = pcall( function () return element[method]( element ) end )
		return ok and result or nil
	end

	local function walk( element, depth )
		while element do
			local ok_id, id = pcall( function () return element.id end )
			if ok_id and id == "ToastNotification" then
				patchContainer( element )
			elseif depth < 8 then
				walk( try( element, "getFirstChild" ), depth + 1 )
			end
			element = try( element, "getNextSibling" )
		end
	end

	if LUI and LUI.roots then
		for _, root in pairs( LUI.roots ) do
			walk( try( root, "getFirstChild" ), 0 )
		end
	end
end )

if not ok then
	tlog( "patch failed: " .. tostring( err ) )
end

-- toast on INVITE GAME, mirroring the open-to-friends toggle toast; when we're the host of a
-- closed match, sending the invite auto-opens it (C++ request_invite), so say that too
pcall( function ()
	require( "ui.uieditor.actions" )

	if cbInviteToastHooked then
		return
	end

	local orig_LobbyInviteFriendGoBack = LobbyInviteFriendGoBack
	if not orig_LobbyInviteFriendGoBack then
		return
	end
	cbInviteToastHooked = true

	LobbyInviteFriendGoBack = function ( menu, element, controller, param, parentMenu )
		-- read before the call: request_invite opens the match as a side effect
		local willOpen = game and game.friendInviteOpensMatch and game.friendInviteOpensMatch()
		local ret = orig_LobbyInviteFriendGoBack( menu, element, controller, param, parentMenu )
		local who = ( param and param.gamertag ) and ( "Invite sent to " .. param.gamertag .. "." ) or "Invite sent."
		CoD.OverlayUtility.ShowToast( "Invite", "INVITE SENT",
			willOpen and ( who .. " Match is now open to friends." ) or who,
			"uie_t7_icon_menu_invite_sent" )
		return ret
	end
end )
