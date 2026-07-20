-- Reroutes the game's native Social JOIN GAME/JOIN PARTY action (SocialJoin) to our own
-- connect logic for CB friends. The native action resolves the friend's session through real
-- Demonware matchmaking, which our launcher-IPC friends don't have, so game.connectToFriend
-- must drive the join instead. The native button's gating/styling/localization is reused as-is.

local function jlog( msg )
	if game and game.friendJoinLog then
		game.friendJoinLog( msg )
	end
end

-- Register the game's own map/gametype id tables into C++ so launcher presence (raw names)
-- resolves to the native ints the friends rich-presence spoof needs. Runs every UI load; idempotent.
pcall( function ()
	if not ( game and game.registerFriendMap and game.registerFriendGametype ) then
		return
	end

	local maps = 0
	if CoD and CoD.mapsTable then
		for name, m in pairs( CoD.mapsTable ) do
			if type( m ) == "table" and m.unique_id then
				game.registerFriendMap( name, tostring( m.unique_id ) )
				maps = maps + 1
			end
		end
	end

	-- game_types has no Lua enumerator; scan ids (the display code does id-keyed lookups the same way)
	local gametypes = 0
	for i = 0, 63 do
		local name = Engine.StructTableLookupString( "game_types", "id", tostring( i ), "name" )
		if name and name ~= "" then
			game.registerFriendGametype( name, tostring( i ) )
			gametypes = gametypes + 1
		end
	end

	jlog( "registered " .. maps .. " maps, " .. gametypes .. " gametypes" )

	-- decode the LobbyQuickJoin gate: the friends-list J key requires the joinable model to
	-- equal one of these; our rich presence sends tJoinable=1
	if Enum and Enum.LobbyJoinable then
		jlog( "LobbyJoinable YES=" .. tostring( Enum.LobbyJoinable.LOBBY_JOINABLE_YES )
			.. " YES_FRIENDS_ONLY=" .. tostring( Enum.LobbyJoinable.LOBBY_JOINABLE_YES_FRIENDS_ONLY ) )
	end
end )

local ok, err = pcall( function ()
	-- actions.lua defines the SocialJoin global; force-load in case we run before it
	require( "ui.uieditor.actions" )

	if cbSocialJoinHooked then
		return
	end

	local orig_SocialJoin = SocialJoin
	if not orig_SocialJoin then
		jlog( "SocialJoin not found; native join reroute not installed" )
		return
	end
	cbSocialJoinHooked = true

	-- signature matches ProcessListAction: (menu, element, controller, actionParam, parentMenu)
	SocialJoin = function ( menu, element, controller, param, parentMenu )
		local xuid = param and param.xuid
		local hex = xuid and Engine.UInt64ToString( xuid )
		if hex and game and game.isFriendJoinable and game.isFriendJoinable( hex ) then
			jlog( "native JOIN rerouted, xuid=" .. hex )
			if game.connectToFriend then
				game.connectToFriend( hex )
			end
			GoBackToMenu( GoBack( menu, controller ), controller, "Lobby" )
			return
		end
		return orig_SocialJoin( menu, element, controller, param, parentMenu )
	end

	jlog( "SocialJoin rerouted to game.connectToFriend for CB friends" )
end )

if not ok then
	jlog( "ERROR: " .. tostring( err ) )
end

-- the engine re-reads ISteamFriends only on Engine.UpdateFriends (Social tab open), so while
-- the menu is open, poke it whenever the launcher pushes a new friends snapshot
local ok2, err2 = pcall( function ()
	require( "ui.uieditor.menus.Social.Social_Main" )

	if cbSocialRefreshHooked then
		return
	end

	local orig_createSocialMain = LUI.createMenu.Social_Main
	if not ( orig_createSocialMain and game and game.friendsSnapshotVersion ) then
		jlog( "Social_Main or friendsSnapshotVersion missing; live refresh not installed" )
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
				jlog( "snapshot v" .. version .. " -> UpdateFriends" )
				Engine.UpdateFriends( controller, Enum.PresenceFilter.PRESENCE_FILTER_ONLINE_AND_IN_TITLE )
			end
		end ) )
		return menu
	end

	jlog( "Social_Main live friends refresh installed" )
end )

if not ok2 then
	jlog( "ERROR (live refresh): " .. tostring( err2 ) )
end

-- toast the host on INVITE GAME, mirroring the open-to-friends toggle toast; when the match
-- was closed, sending the invite auto-opens it (C++ request_invite), so say that too
local ok3, err3 = pcall( function ()
	require( "ui.uieditor.actions" )

	if cbInviteToastHooked then
		return
	end

	local orig_LobbyInviteFriendGoBack = LobbyInviteFriendGoBack
	if not orig_LobbyInviteFriendGoBack then
		jlog( "LobbyInviteFriendGoBack not found; invite toast not installed" )
		return
	end
	cbInviteToastHooked = true

	LobbyInviteFriendGoBack = function ( menu, element, controller, param, parentMenu )
		local wasOpen = Dvar.nat_open and Dvar.nat_open:get()
		local ret = orig_LobbyInviteFriendGoBack( menu, element, controller, param, parentMenu )
		local who = ( param and param.gamertag ) and ( "Invite sent to " .. param.gamertag .. "." ) or "Invite sent."
		CoD.OverlayUtility.ShowToast( "Invite", "INVITE SENT",
			wasOpen and who or ( who .. " Match is now open to friends." ),
			"uie_t7_icon_menu_invite_sent" )
		return ret
	end

	jlog( "invite toast installed" )
end )

if not ok3 then
	jlog( "ERROR (invite toast): " .. tostring( err3 ) )
end
