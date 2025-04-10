if not Ryzhide then
	-- global Ryzhide class
	Ryzhide = {
		main_window_name = "ui:interface:hide_n_hype_main",
		timer_str = "@UI:VARIABLES:CURRENT_SERVER_TICK",
		pull_data = 10,
		request_server = 0,
		json_data_ready = 0,
		need_json_update = 1,
		hide_n_hide_json_data = nil,
		hidenhype_feedback = nil,
		load_animation_timer = 0,
		load_animation = 0,
		player_already_registerd = 0,
		game_is_full_loaded = 0,
		
		player_click_to_register = 0,
		register_player_online = 0,
		regist_as_hunter = 0,
		regist_as_hunter_or_most_wanted = 0,
		regist_as_most_wanted = 0,
		regist_as_reject = 0,

		hunter_array = 0,
		hunter_or_most_wanted_array = 0,
		most_wanted_array = 0,
		reject_array = 0,
		
		reject_invite_round_id = 0,

		hunter_array_color = "255 0 0 255",
		hunter_or_most_wanted_array_color = "255 0 0 255",
		most_wanted_array_color = "255 0 0 255",
		reject_array_color = "0 255 0 255",

		hunter_accept_image = "",
		hunter_or_most_wanted_accept_image = "",
		most_wanted_accept_image = "",

		hunter_accept_active = "true",
		hunter_or_most_wanted_accept_active = "true",
		most_wanted_accept_active = "true",

		hunter_ok_false_image = "",
		hunter_or_most_wanted_ok_false_image = "",
		most_wanted_ok_false_image = "",

		current_round_start = 0,
		current_round_id = 0,

		player_name = "",
		
		json_pull_counter = 0,
		manuell_action = 0,
		
		round_time_to_start = 0,
		spawn_a_object = 0,
		save_x_pos = 0,
		save_y_pos = 0,
		save_z_pos = 0,
		player_type = "",
		
		current_round_end = 0,
		timer_hint_1 = 0,
		timer_hint_2 = 0,
		timer_hint_3 = 0,

		hint_1_text = "????",
		hint_2_text = "????",
		hint_3_text = "????",
		pull_request_data = 0,
		
		reward_already_claimed = 0,
		closed_most_wanted_not_found_hunter = 0,
		closed_loos_hunter = 0,
		
		target_name_old = ""
	}
end

function Ryzhide:update_timer(id,remaining_time_display,duration_display)
	local mainui = getUI(self.main_window_name)
	local timer_hard_text = mainui:find(id)

	local minutes = math.floor(remaining_time_display / 60)
	local seconds = remaining_time_display % 60
	
	timer_hard_text:find("text").hardtext = string.format("%02d:%02d", minutes, seconds)
	
	if(remaining_time_display <= duration_display * 0.5 and remaining_time_display > duration_display * 0.1)then
		--change coloro to orange by 50% or less but bigger as 10%
		timer_hard_text:find("text").color = "255 165 0 255"
	elseif(remaining_time_display <= duration_display * 0.1)then
		--change coloro to red by 10% or less
		timer_hard_text:find("text").color = "255 0 0 255"
	elseif(remaining_time_display > duration_display * 0.5)then
		--change coloro to green by more as 50%
		timer_hard_text:find("text").color = "0 185 0 255"
	end
end

function Ryzhide:position_json_load_window()
	local json_load_window = getUI("ui:interface:load_pars_json")
	local mainui = getUI(self.main_window_name)
	
	main_window_x = mainui.x
	main_window_y = mainui.y
	main_window_h = mainui.h
	main_window_w = mainui.w
	
	json_load_window.x = main_window_x + main_window_w
	json_load_window.y = main_window_y
	
	if(self.load_animation_timer == 25)then
		self.load_animation_timer = 0
		if(self.load_animation == 0)then
			json_load_window:find("display_loading_img").texture="ico_time.tga"
			self.load_animation = 1
		elseif(load_animation == 1)then
			json_load_window:find("display_loading_img").texture="ico_time_right.tga"
			self.load_animation = 2
		elseif(load_animation == 2)then
			json_load_window:find("display_loading_img").texture="ico_time_buttom.tga"
			self.load_animation = 3
		else
			json_load_window:find("display_loading_img").texture="ico_time_left.tga"
			self.load_animation = 0
		end
	else
		self.load_animation_timer = self.load_animation_timer + 1
	end
end

function Ryzhide:start_fetch_json_data(url_to_fetch)
	debug("start_fetch_data: "..url_to_fetch)
	self.pull_data = 10
	self.request_server = 0
	self.json_data_ready = 0
	self.load_animation = 0
	self.load_animation_timer = 0
	
	framewin = getUI("ui:interface:load_pars_json")
	setOnDraw(framewin, "Ryzhide:fetch_json_data('"..url_to_fetch.."')")
	framewin.active=true
end

function Ryzhide:fetch_json_data(url_to_load)
	--run every 2 secound
	Ryzhide:position_json_load_window()
	if(self.pull_data == 10)then
		self.pull_data = 0
		if(self.request_server == 0)then
			self.request_server = 1
			self.json_data_ready = 0
			self.hidenhype_feedback = nil
			
			webig:openUrlInBg(url_to_load)
		end
	else
		self.pull_data = self.pull_data + 1
	end
	
	if(self.request_server == 1)then
		if(self.hidenhype_feedback ~= nil)then
			self.request_server = 0
			self.hide_n_hide_json_data = nil
			self.hide_n_hide_json_data = Json.decode(self.hidenhype_feedback)
			
			if (self.hide_n_hide_json_data) then
				if (self.hide_n_hide_json_data.error) then
					debug("Load Json Process ' error: " .. self.hide_n_hide_json_data.error.."'")
					debug("Load Json Process ' current_time: " .. self.hide_n_hide_json_data.current_time.."'")
				elseif (self.hide_n_hide_json_data.success) then
					debug("Load Json Process 'success: " .. self.hide_n_hide_json_data.success.."'")
					debug("Load Json Process 'current_time: " .. self.hide_n_hide_json_data.current_time.."'")
				else
					debug("Load Json Process 'no error or success found'")
				end
				self.json_data_ready = 1
			else
				debug("Load Json Process 'JSON-Decode failed'")
				self.json_data_ready = 0
			end
		end
	end
	
	if(self.json_data_ready == 1)then
		Ryzhide:close_window("ui:interface:load_pars_json", "")
	end
end

function Ryzhide:display_message_to_player(msg_type, msg_option, msg_text)
	local msg = ucstring()
	local msg_color = ""
	
	if(msg_type == "error")then
		--error red
		msg_color="F00F"
	else
		--other yellow
		msg_color="FF0F"
	end
	
	msg:fromUtf8("@{"..msg_color.."}"..msg_text)
	displaySystemInfo(msg, msg_option)
end

function Ryzhide:open_resize_main_window(win_h, win_w, render_html_content)
	local mainui = getUI(self.main_window_name)
	local mainui_html = mainui:find("html")
	
	if(mainui.active == false)then
		mainui.active = true
	end
	
	if(mainui.opened == false)then
		mainui.opened = true
	end
	
	mainui.h = win_h
	mainui.w = win_w

	mainui_heade_open = mainui:find("header_opened")
	mainui_heade_open.h = win_h
	mainui_heade_open.w = win_w
	
	mainui_html:renderHtml(render_html_content)
end

function Ryzhide:close_window(window_id, function_id)
	if(function_id == "abort_because_do_nothing")then
		removeOnDbChange(getUI(self.main_window_name),self.timer_str)
	end
	
	getUI(window_id).active=false
end

function Ryzhide:click_close_button(window_id)
	if(window_id == "close_window_build_most_wanted_not_found_hunter")then
		self.closed_most_wanted_not_found_hunter = 1
	end
	
	if(window_id == "close_window_build_loos_hunter")then
		self.closed_loos_hunter = 1
	end
	
	getUI(self.main_window_name).active=false
end

function Ryzhide:load_translation(translation_var)
	return Ryzhide:htmlentities(translation_var)
end

function Ryzhide:htmlentities(text)
	local html_trans_content = ""
	local html_client_translation = ""
	
	html_client_translation = tostring(i18n.get(text):toUtf8())

	html_trans_content = html_client_translation:gsub("<NotExist:", "{")
	html_trans_content = html_trans_content:gsub(">", "}")
	html_trans_content = html_trans_content:gsub("'", "`")
	
	return  tostring(html_trans_content)
end

function Ryzhide:check_local_player_name()
	if(self.player_name == "" or self.player_name == nil)then
		self.player_name = getPlayerName()
	end
end

function Ryzhide:toUnixTimestamp(dateString)
	local pattern = "(%d+)-(%d+)-(%d+) (%d+):(%d+):(%d+)"
	local year, month, day, hour, min, sec = dateString:match(pattern)

	return os.time({
		year = tonumber(year),
		month = tonumber(month),
		day = tonumber(day),
		hour = tonumber(hour),
		min = tonumber(min),
		sec = tonumber(sec)
	})
end

function Ryzhide:distance_calc(x1, y1, z1, x2, y2, z2)
	if(x1 == nil or y1 == nil or z1 == nil or x2 == nil or y2 == nil or z2 == nil)then
		debug("calc_distand_a_nil_value")
		return 999
	end
	
	local dx = x2 - x1
	local dy = y2 - y1
	local dz = z2 - z1
	return math.sqrt(dx * dx + dy * dy + dz * dz)
end

--###################################### START load and start at login function #######################################


function Ryzhide:check_data_at_login()
	local mainui = getUI(self.main_window_name)
	self.need_json_update = 1
	self.json_data_ready = 0
	
	addOnDbChange(mainui, self.timer_str, "Ryzhide:pars_json_data_login()")
end

function Ryzhide:pars_json_data_login()
	if(self.need_json_update == 1)then
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=jsonData&need_json_data=login')
	end
	
	if(self.json_data_ready == 1)then
		if (self.hide_n_hide_json_data) then
			if (self.hide_n_hide_json_data.error) then
				debug(self.hide_n_hide_json_data.error)
				
				--player is not registrated
				if(self.hide_n_hide_json_data.error == "not_registerd")then
					self.player_already_registerd = 0
				end
				
				Ryzhide:login_data_ok_process_error()
				
			elseif (self.hide_n_hide_json_data.success) then
				self.game_is_full_loaded = 1
				self.reward_already_claimed = 0
				
				--set flag player is registrated
				self.player_already_registerd = 1
				
				debug("ready: "..self.hide_n_hide_json_data.ready)
				debug("player_round_id: "..self.hide_n_hide_json_data.player_round_id)
				debug("round_id: "..self.hide_n_hide_json_data.round_id)
				
				--player already take rewards dont open the window again
				if(tonumber(self.hide_n_hide_json_data.ready) == 69 and tonumber(self.hide_n_hide_json_data.player_round_id) == tonumber(self.hide_n_hide_json_data.round_id))then
					self.reward_already_claimed = 1
				end
				
				
				--player reject play this round
				if(tonumber(self.hide_n_hide_json_data.ready) == 4 and tonumber(self.hide_n_hide_json_data.player_round_id) == tonumber(self.hide_n_hide_json_data.round_id))then
					self.reject_invite_round_id = tonumber(self.hide_n_hide_json_data.player_round_id)
					self.current_round_id = tonumber(self.hide_n_hide_json_data.round_id)
					Ryzhide:login_data_ok_process()
				end
				
				if(self.hide_n_hide_json_data.game_status == "asked_to_join")then
					 Ryzhide:open_ask_for_join_window(self.hide_n_hide_json_data.current_round_start, self.hide_n_hide_json_data.round_id)
				end
				
				if(self.hide_n_hide_json_data.game_status == "start_new_round")then
					if(tonumber(self.hide_n_hide_json_data.ready) == 0 and tonumber(self.hide_n_hide_json_data.player_round_id) == tonumber(self.hide_n_hide_json_data.round_id))then
						self.reject_invite_round_id = tonumber(self.hide_n_hide_json_data.player_round_id)
						self.current_round_id = tonumber(self.hide_n_hide_json_data.round_id)
						Ryzhide:login_data_ok_process()
					else
						Ryzhide:start_new_round(self.hide_n_hide_json_data.current_round_start, self.hide_n_hide_json_data.most_wanted, self.hide_n_hide_json_data.round_id)
					end
				end
				
				if(self.hide_n_hide_json_data.game_status == "game_is_running")then
					if(tonumber(self.hide_n_hide_json_data.ready) == 0 and tonumber(self.hide_n_hide_json_data.player_round_id) == tonumber(self.hide_n_hide_json_data.round_id))then
						self.reject_invite_round_id = tonumber(self.hide_n_hide_json_data.player_round_id)
						self.current_round_id = tonumber(self.hide_n_hide_json_data.round_id)
						Ryzhide:login_data_ok_process()
					else
						Ryzhide:start_the_hide_now(self.hide_n_hide_json_data.current_round_end, self.hide_n_hide_json_data.most_wanted, self.hide_n_hide_json_data.round_id)
					end
				end
				
				--status waiting, so close window and wait for global broadcatse
				if(self.hide_n_hide_json_data.game_status == "waiting")then
					Ryzhide:login_data_ok_process()
				end
			else
				debug("no 'error' or 'success' found")
			end
		else
			debug("JSON-Decode failed")
		end
		
		self.need_json_update = 1
	end
end

function Ryzhide:login_data_ok_process()
	local mainui = getUI(self.main_window_name)
	
	self.json_data_ready = 0

	debug("Ryzhide success: "..self.hide_n_hide_json_data.success)
	debug("Ryzhide game_status: "..self.hide_n_hide_json_data.game_status)
	debug("Ryzhide round_id: "..self.hide_n_hide_json_data.round_id)
	
	removeOnDbChange(mainui, self.timer_str)
	
	mainui.active = false
end

function Ryzhide:login_data_ok_process_error()
	local mainui = getUI(self.main_window_name)
	removeOnDbChange(mainui, self.timer_str)
	mainui.active = false
	self.game_is_full_loaded = 1
end

--###################################### END load and start at login function #######################################


--###################################### START build and load registration window #######################################

function Ryzhide:build_register_window()
	local window_height = 250
	local window_width = 365
	local table_width = window_width - 15
	
	local html_ungister = ""
	html_ungister=[[<title>]]..Ryzhide:load_translation("hide_n_hype_unregister_window")..[[</title>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="right"><div id="close_window_unregister_window" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_unregister_window;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype")..[[</h2></td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_desc")..[[</td>
			</tr>
			
			<tr>
				<td>&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_asked_unregister")..[[</td>
			</tr>
			
			<tr>
				<td>&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_unregister_now_and_close")..[[</td>
			</tr>
			
			<tr>
				<td align="center"><div id="reject_accept" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:reject_accept;bg:ico_opening_hit.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_unregister")..[[;lua_function:click_icon_registration;'></div></td>
			</tr>
			</table>]]
	
	local html_register = ""
	html_register=[[<title>]]..Ryzhide:load_translation("hide_n_hype_register_window")..[[</title>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="right"><div id="close_window_register_window" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_register_window;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype")..[[</h2></td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_desc")..[[</td>
			</tr>
			
			<tr>
				<td>&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_asked_register")..[[</td>
			</tr>
			
			<tr>
				<td>&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_register_now")..[[</td>
			</tr>
			
			<tr>
				<td align="center"><div id="register_accept" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:register_accept;bg:ico_dodge.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_register")..[[;lua_function:click_icon_registration;'></div></td>
			</tr>
			</table>]]
			
	if(self.player_already_registerd == 1)then
		Ryzhide:open_resize_main_window(window_height, window_width, html_ungister)
	else
		Ryzhide:open_resize_main_window(window_height, window_width, html_register)
	end
end

function Ryzhide:open_register_window()
	local mainui = getUI(self.main_window_name)
	Ryzhide:build_register_window()
	
	self.json_data_ready = 0
	addOnDbChange(mainui, self.timer_str, "Ryzhide:pars_json_data_register()")
end

function Ryzhide:display_register_sucess(sucess_msg)
	self.json_data_ready = 0

	Ryzhide:display_message_to_player('info', 'AMB', sucess_msg)
end

function Ryzhide:display_register_error(error_msg)
	self.json_data_ready = 0
	
	--unlock register button again
	local mainui_group = getUI(self.main_window_name):find("register_accept")
	mainui_group:find("img1").texture = ""
	mainui_group:find("ctrl").active = true
	
	Ryzhide:display_message_to_player('error', 'AMB', error_msg)
end

function Ryzhide:display_unregister_error(error_msg)
	self.json_data_ready = 0
	
	--unlock register button again
	local mainui_group = getUI(self.main_window_name):find("reject_accept")
	mainui_group:find("img1").texture = ""
	mainui_group:find("ctrl").active = true
	
	Ryzhide:display_message_to_player('error', 'AMB', error_msg)
end

function Ryzhide:pars_json_data_register()
	if(self.player_already_registerd == 0)then
		if(self.json_data_ready == 1)then
			if (self.hide_n_hide_json_data) then
				if (self.hide_n_hide_json_data.error) then
					if(self.hide_n_hide_json_data.error == "already_registerd")then
						Ryzhide:display_register_sucess(Ryzhide:load_translation("hide_n_hype_already_registerd"))
						self.player_already_registerd = 1
						Ryzhide:build_register_window()
					else
						display_register_error(self.hide_n_hide_json_data.error)
					end
				elseif (self.hide_n_hide_json_data.success) then
					Ryzhide:display_register_sucess(Ryzhide:load_translation("hide_n_hype_reg_sucesfully"))
					self.player_already_registerd = 1
					Ryzhide:build_register_window()
				else
					Ryzhide:display_register_error("no 'error' or 'success' found")
				end
			else
				Ryzhide:display_register_error("JSON-Decode failed")
			end
		end
	else
		if(self.json_data_ready == 1)then
			if (self.hide_n_hide_json_data) then
				if (self.hide_n_hide_json_data.error) then
					if(self.hide_n_hide_json_data.error == "not_registerd")then
						Ryzhide:display_unregister_error(Ryzhide:load_translation("hide_n_hype_not_registerd"))
						self.player_already_registerd = 0
						Ryzhide:build_register_window()
					else
						Ryzhide:display_unregister_error(self.hide_n_hide_json_data.error)
					end
				elseif (self.hide_n_hide_json_data.success) then
					Ryzhide:display_register_sucess(Ryzhide:load_translation("hide_n_hype_unreg_sucesfully"))
					self.player_already_registerd = 0
					Ryzhide:build_register_window()
				else
					Ryzhide:display_unregister_error("no 'error' or 'success' found")
				end
			else
				Ryzhide:display_unregister_error("JSON-Decode failed")
			end
		end
	end
end


function Ryzhide:click_icon_registration(id)
	local mainui = getUI(self.main_window_name)

	if(id == "close_window")then
		removeOnDbChange(mainui, self.timer_str)
		Ryzhide:close_window(self.main_window_name, "")
		return
	end
	
	if(id == "register_accept")then
		debug("start_register_now")
		Ryzhide:start_fetch_json_data("https://app.ryzom.com/app_ryzhide/?mode=register")
	end
	
	if(id == "reject_accept")then
		debug("start_unregister_now")
		Ryzhide:start_fetch_json_data("https://app.ryzom.com/app_ryzhide/?mode=unregister")
	end
	
	local mainui_group = getUI(self.main_window_name):find(id)
	local image_ui_controll = mainui_group:find("img1").texture
	if(image_ui_controll == "")then
		mainui_group:find("img1").texture = "curs_rotate.png"
		mainui_group:find("ctrl").active = false
	end
end

--###################################### END build and load registration window #######################################


--###################################### START build and load invite window #######################################
function Ryzhide:build_invite_window()
	local window_height = 300
	local window_width = 365
	local table_width = window_width - 15
	
	local html_invite_window=""
	html_invite_window=[[<title>]]..Ryzhide:load_translation("hide_n_hype_a_new_hide_start_soon_window")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="center"><img src="ico_time.png" width="40"></td>
				<td align="center" colspan="2"><div id="hide_n_hype_timer_invite" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_invite;timer:01:00;'></div></td>
				<td align="center"><img src="ico_time.png" width="40"></td>
			</tr>
			
			<tr>
				<td>&nbsp;</td>
				<td colspan="2" align="center">]]..Ryzhide:load_translation("hide_n_hype_currently_registerd_player_online")..[[</td>
				<td>&nbsp;</td>
			</tr>
			
			<tr>
				<td>&nbsp;</td>
				<td colspan="2" align="center"><div id="player_online_text" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:player_online_text;font_size:13;text_color:255 0 0 255;hardtext:]]..self.register_player_online..[[;'></div></td>
				<td>&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_hunter")..[[</td>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_hunter_and_most_wanted")..[[</td>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_most_wanted")..[[</td>
				<td align="center">]]..Ryzhide:load_translation("hide_n_hype_reject")..[[</td>
			</tr>
			
			<tr>
				<td colspan="4">&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center"><div id="hunte_accept" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:hunte_accept;bg:tag_rpbg8.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_hunter")..[[;lua_function:click_icon_ask_for_join;img1:]]..self.hunter_accept_image..[[;ctrl_active:]]..self.hunter_accept_active..[[;'></div></td>
				<td align="center"><div id="hunter_or_mostwanted_accept" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:hunter_or_mostwanted_accept;bg:ico_feint.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_hunter_or_mostwanted")..[[;lua_function:click_icon_ask_for_join;img1:]]..self.hunter_or_most_wanted_accept_image..[[;ctrl_active:]]..self.hunter_or_most_wanted_accept_active..[[;'></div></td>
				<td align="center"><div id="mostwanted_accept" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:mostwanted_accept;bg:ico_dodge.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_mostwanted")..[[;lua_function:click_icon_ask_for_join;img1:]]..self.most_wanted_accept_image..[[;ctrl_active:]]..self.most_wanted_accept_active..[[;'></div></td>
				<td align="center"><div id="reject_accept" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:reject_accept;bg:ico_opening_hit.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_Reject")..[[;lua_function:click_icon_ask_for_join;'></div></td>
			</tr>
			
			<tr>
				<td colspan="4">&nbsp;</td>
			</tr>
			
			<tr>
				<td align="center"><div id="hunte_text" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hunte_text;font_size:13;text_color:]]..self.hunter_array_color..[[;hardtext:1 / ]]..self.hunter_array..[[;'></div></td>
				<td align="center"><div id="hunter_or_mostwanted_text" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hunte_text;font_size:13;text_color:]]..self.hunter_or_most_wanted_array_color..[[;hardtext:1 / ]]..self.hunter_or_most_wanted_array..[[;'></div></td>
				<td align="center"><div id="mostwanted_text" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hunte_text;font_size:13;text_color:]]..self.most_wanted_array_color..[[;hardtext:1 / ]]..self.most_wanted_array..[[;'></div></td>
				<td align="center"><div id="reject_text" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hunte_text;font_size:13;text_color:]]..self.reject_array_color..[[;hardtext:]]..self.reject_array..[[;'></div></td>
			</tr>
			<tr>
				<td align="center"><div id="hunte_display_status" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_only_images;id:hunte_display_status;img1:]]..self.hunter_ok_false_image..[[;tooltip:]]..Ryzhide:load_translation("hide_n_hype_not_enough")..[[;'></div></td>
				<td align="center"><div id="hunter_or_mostwanted_display_status" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_only_images;id:hunter_or_mostwanted_display_status;img1:]]..self.hunter_or_most_wanted_ok_false_image..[[;tooltip:]]..Ryzhide:load_translation("hide_n_hype_not_enough")..[[;'></div></td>
				<td align="center"><div id="mostwanted_display_status" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_only_images;id:mostwanted_display_status;img1:]]..self.most_wanted_ok_false_image..[[;tooltip:]]..Ryzhide:load_translation("hide_n_hype_not_enough")..[[;'></div></td>
				<td align="center">&nbsp;</td>
			</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_invite_window)
end


function Ryzhide:build_wait_start_window()
	local window_height = 45
	local window_width = 380
	local table_width = window_width - 15

	local html_wait_for_start=""
	html_wait_for_start=[[<title></title>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="center"><h1>]]..Ryzhide:load_translation("hide_n_hype_wait_for_game_start")..[[</h1></td>
			</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_wait_for_start)
end

function Ryzhide:open_ask_for_join_window(current_round_time,current_round)
	if(self.game_is_full_loaded == 0)then
		debug("game_not_loaded -- open_ask_for_join_window")
		return
	end
	
	if(self.player_already_registerd == 0)then
		return
	end
	
	self.current_round_id = tonumber(current_round)
	
	debug("asked_join_id: "..current_round)
	debug("reject_invite_round_id: "..self.reject_invite_round_id)
	
	if(tonumber(self.current_round_id) == tonumber(self.reject_invite_round_id))then
		debug("player_already_reject_dont_open_window_again")
		return
	else
		self.reject_invite_round_id = 0
	end
	
	local mainui = getUI(self.main_window_name)
	
	self.player_click_to_register = 0
	self.need_json_update = 1
	self.json_data_ready = 0
	self.current_round_start = 0
	self.json_pull_counter = 0
	self.reward_already_claimed = 0
	
	Ryzhide:check_local_player_name()
	
	addOnDbChange(mainui, self.timer_str, "Ryzhide:pars_json_data_ask_for_join()")
end

function Ryzhide:display_asked_for_join_sucess(sucess_msg)
	self.json_data_ready = 0
	Ryzhide:display_message_to_player('info', 'AMB', sucess_msg)
end

function Ryzhide:display_asked_for_join_error(error_msg)
	self.json_data_ready = 0
	
	--unlock register button again
	local ui_register_button = getUI(self.main_window_name):find("register_accept")
	ui_register_button:find("img1").texture = ""
	ui_register_button:find("ctrl").active = true
	
	Ryzhide:display_message_to_player('error', 'AMB', error_msg)
end

function Ryzhide:invite_timer_stopped()
	removeOnDbChange(getUI(self.main_window_name),self.timer_str)
	
	debug("Timer_is_over!")
	self.need_json_update = 1
	self.json_data_ready = 0
	self.current_round_start = 0
	
	if(self.player_click_to_register == 0)then
		Ryzhide:close_window(self.main_window_name, "abort_because_do_nothing")
		self.reject_invite_round_id = tonumber(self.current_round_id)
		return
	else
		Ryzhide:build_wait_start_window()
	end
end

function Ryzhide:pars_json_data_ask_for_join()
		if(self.need_json_update == 1)then
			self.need_json_update = 0
			Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=jsonData&need_json_data=asked_to_join')
		end
		
		if(self.current_round_start ~= 0 and self.json_pull_counter > 2)then
			local remaining_time = self.current_round_start - os.time()
			
			if(remaining_time < 0)then
				Ryzhide:invite_timer_stopped()
			else
				Ryzhide:update_timer("hide_n_hype_timer_invite",remaining_time, (1*60))
			end
		end

		if(self.json_data_ready == 1)then
			if (self.hide_n_hide_json_data) then
				if (self.hide_n_hide_json_data.error) then
					if(self.hide_n_hide_json_data.error == "not_registerd")then
						Ryzhide:display_unregister_error(Ryzhide:load_translation("hide_n_hype_not_registerd"))
					else
						Ryzhide:display_asked_for_join_error(self.hide_n_hide_json_data.error)
					end
				elseif (self.hide_n_hide_json_data.success) then
					if(self.hide_n_hide_json_data.success == "asked_to_join")then
						self.json_pull_counter = self.json_pull_counter + 1
						--Ryzhide:display_asked_for_join_sucess(self.hide_n_hide_json_data.success)
						self.json_data_ready = 0
						
						if(self.json_pull_counter == 1)then
							Ryzhide:inti_invite_window(self.hide_n_hide_json_data)
						elseif(self.json_pull_counter == 2)then
							Ryzhide:build_invite_window(1212,2)
						elseif(self.json_pull_counter > 2)then
							if(self.manuell_action == 0)then
								Ryzhide:update_invite_window(self.hide_n_hide_json_data)
							end
						end
					elseif(self.hide_n_hide_json_data.success == "ok_player_accept")then
						local message_as = ""
						if(self.hide_n_hide_json_data.accept_as == "hunter")then
							message_as = "hide_n_hype_hunter"
						end
						if(self.hide_n_hide_json_data.accept_as == "hunter_and_most_wanted")then
							message_as = "hide_n_hype_hunter_and_most_wanted"
						end
						if(self.hide_n_hide_json_data.accept_as == "most_wanted")then
							message_as = "hide_n_hype_most_wanted"
						end
						if(self.hide_n_hide_json_data.accept_as == "reject")then
							message_as = "hide_n_hype_reject"
						end
						local message = Ryzhide:load_translation("hide_n_hype_ok_player_accept").." : "..Ryzhide:load_translation(message_as)
						Ryzhide:display_asked_for_join_sucess(message)
						self.manuell_action = 0
					end
				else
					Ryzhide:display_unregister_error("no 'error' or 'success' found")
				end
			else
				Ryzhide:display_unregister_error("JSON-Decode failed")
			end
			
			self.need_json_update = 1
		end
end

function Ryzhide:inti_invite_window(json_data)
	debug("inti_invite_window: "..json_data.current_round_start)
	self.register_player_online = json_data.reg_online_player
	
	Ryzhide:check_local_player_name()
	
	if(json_data.current_round_start == "0000-00-00 00:00:00")then
		self.current_round_start = 0
		self.json_pull_counter = 0
	else	
		self.current_round_start = Ryzhide:toUnixTimestamp(json_data.current_round_start)
	end
	
	if (type(json_data.hunter_list) == "boolean") then
		self.regist_as_hunter = 0
		self.hunter_accept_image = ""
	else
		self.hunter_array = 0
		for _, name in ipairs(json_data.hunter_list) do
			self.hunter_array = self.hunter_array + 1
			if(self.player_name == name)then
				self.regist_as_hunter = 1
				self.hunter_accept_image = "rap_invited_no_dm.tga"
				self.hunter_accept_active = "false"
			end
		end
	end
	
	if(self.hunter_array == 0)then
		self.hunter_ok_false_image = "w_answer_16_cancel.png"
		self.hunter_array_color = "255 0 0 255"
	else
		self.hunter_ok_false_image = "rap_invited_no_dm.png"
		self.hunter_array_color = "0 255 0 255"
	end
	
	if (type(json_data.hunter_and_most_wanted_list) == "boolean") then
		self.regist_as_hunter_or_most_wanted = 0
		self.hunter_or_most_wanted_accept_image = ""
	else
		self.hunter_or_most_wanted_array = 0
		for _, name in ipairs(json_data.hunter_and_most_wanted_list) do
			self.hunter_or_most_wanted_array = self.hunter_or_most_wanted_array + 1
			if(self.player_name == name)then
				self.regist_as_hunter_or_most_wanted = 1
				self.hunter_or_most_wanted_accept_image = "rap_invited_no_dm.tga"
				self.hunter_or_most_wanted_accept_active = "false"
			end
		end
	end
	
	if(self.hunter_or_most_wanted_array == 0)then
		self.hunter_or_most_wanted_ok_false_image = "w_answer_16_cancel.png"
		self.hunter_or_most_wanted_array_color = "255 0 0 255"
	else
		self.hunter_or_most_wanted_ok_false_image = "rap_invited_no_dm.png"
		self.hunter_or_most_wanted_array_color = "0 255 0 255"
	end
	
	if (type(json_data.most_wanted_list) == "boolean") then
		self.regist_as_most_wanted = 0
		self.most_wanted_accept_image = ""
	else
		self.most_wanted_array = 0
		for _, name in ipairs(json_data.most_wanted_list) do
			self.most_wanted_array = self.most_wanted_array + 1
			if(self.player_name == name)then
				self.regist_as_most_wanted = 1
				self.most_wanted_accept_image = "rap_invited_no_dm.tga"
				self.most_wanted_accept_active = "false"
			end
		end
	end
	
	if(self.most_wanted_array == 0)then
		self.most_wanted_ok_false_image = "w_answer_16_cancel.png"
		self.most_wanted_array_color = "255 0 0 255"	
	else
		self.most_wanted_ok_false_image = "rap_invited_no_dm.png"
		self.most_wanted_array_color = "0 255 0 255"
	end
	
	if (type(json_data.reject_list) == "boolean") then
		self.regist_as_reject = 0
	else
		self.reject_array = 0
		for _, name in ipairs(json_data.reject_list) do
			self.reject_array = self.reject_array + 1
			if(self.player_name == name)then
				self.regist_as_reject = 1
			end
		end
	end
	
	if(self.reject_array == 0)then
		self.reject_array_color = "0 255 0 255"
	else
		self.reject_array_color = "255 0 0 255"
	end
end

function Ryzhide:update_invite_window(json_data)
	debug("update_invite_window: "..json_data.current_round_start)
	
	if(json_data.current_round_start == "0000-00-00 00:00:00")then
		self.current_round_start = 0
		self.json_pull_counter = 0
	else	
		self.current_round_start = Ryzhide:toUnixTimestamp(json_data.current_round_start)
	end
	
	local mainui = getUI(self.main_window_name)
	
	local player_online_text = mainui:find("player_online_text")
	player_online_text:find("text").hardtext = json_data.reg_online_player
	
	Ryzhide:check_and_pars_player_infos(json_data.hunter_list,"hunte_accept","hunte_text","hunte_display_status")
	Ryzhide:check_and_pars_player_infos(json_data.hunter_and_most_wanted_list,"hunter_or_mostwanted_accept","hunter_or_mostwanted_text","hunter_or_mostwanted_display_status")
	Ryzhide:check_and_pars_player_infos(json_data.most_wanted_list,"mostwanted_accept","mostwanted_text","mostwanted_display_status")
	Ryzhide:check_and_pars_player_infos(json_data.reject_list,"reject_accept","reject_text","none")
end

function Ryzhide:check_and_pars_player_infos(json_data,action_button,action_count_text,action_ok_image)
	local mainui = getUI(self.main_window_name)
	local count_array = 0
	
	Ryzhide:check_local_player_name()
	
	if (type(json_data) == "boolean") then
		local regist_hunter_img = mainui:find(action_button)
		regist_hunter_img:find("img1").texture = ""
		regist_hunter_img:find("ctrl").active = true
		
		local regist_hunter_text = mainui:find(action_count_text)
		if(action_count_text == "reject_text")then
			regist_hunter_text:find("text").hardtext = "0"
			regist_hunter_text:find("text").color = "0 255 0 255"
		else
			regist_hunter_text:find("text").hardtext = "1 / 0"
			regist_hunter_text:find("text").color = "255 0 0 255"
		end
	else
		count_array = 0
		local regist_hunter_img = mainui:find(action_button)
		regist_hunter_img:find("img1").texture = ""
		regist_hunter_img:find("ctrl").active = true
		
		for _, name in ipairs(json_data) do
			count_array = count_array + 1
			if(self.player_name == name)then
				debug("player_name: "..self.player_name.." array_name: "..name)
				regist_hunter_img:find("img1").texture = "rap_invited_no_dm.tga"
				regist_hunter_img:find("ctrl").active = false
			end
		end
	end
	
	if(count_array == 0)then
		if(action_ok_image ~= "none")then
			local regist_hunter_ok_img = mainui:find(action_ok_image)
			regist_hunter_ok_img:find("img1").texture = "w_answer_16_cancel.png"
			regist_hunter_ok_img:find("ctrl").tooltip = Ryzhide:load_translation("hide_n_hype_not_enough")
		end
	else
		local regist_hunter_text = mainui:find(action_count_text)
		if(action_count_text == "reject_text")then
			regist_hunter_text:find("text").hardtext = count_array
		else
			regist_hunter_text:find("text").hardtext = "1 / "..count_array
		end
		
		if(action_count_text == "reject_text")then
			regist_hunter_text:find("text").color = "255 0 0 255"
		else
			regist_hunter_text:find("text").color = "0 255 0 255"
		end
		
		if(action_ok_image ~= "none")then
			local regist_hunter_ok_img = mainui:find(action_ok_image)
			regist_hunter_ok_img:find("img1").texture = "rap_invited_no_dm.png"
			regist_hunter_ok_img:find("ctrl").tooltip = Ryzhide:load_translation("hide_n_hype_ok_enough_player")
		end
	end
end

function Ryzhide:click_icon_ask_for_join(id)
	if(id == "hunte_accept")then
		self.player_click_to_register = 1
		self.manuell_action = 1
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=accept_invite&accept_as=hunter')
	end
	
	if(id == "hunter_or_mostwanted_accept")then
		self.player_click_to_register = 1
		self.manuell_action = 1
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=accept_invite&accept_as=hunter_and_most_wanted')
	end
	
	if(id == "mostwanted_accept")then
		self.player_click_to_register = 1
		self.manuell_action = 1
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=accept_invite&accept_as=most_wanted')
	end
	
	if(id == "reject_accept")then
		self.player_click_to_register = 1
		self.manuell_action = 1
		self.need_json_update = 0
		self.reject_invite_round_id = tonumber(self.current_round_id)
		
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=accept_invite&accept_as=reject')
		removeOnDbChange(getUI(self.main_window_name),self.timer_str)
		Ryzhide:close_window(self.main_window_name, "")
		return
	end

	local mainui_group = getUI(self.main_window_name):find(id)
	local image_ui_controll = mainui_group:find("img1").texture
	if(image_ui_controll == "")then
		mainui_group:find("img1").texture = "curs_rotate.png"
		mainui_group:find("ctrl").active = false
	end
end

--###################################### END build and load invite window #######################################




--###################################### START build and load prepartion window #######################################


function Ryzhide:start_new_round(time_to_start, most_wanted_name, current_round_id)
	if(self.game_is_full_loaded == 0)then
		debug("game_not_loaded -- start_new_round")
		return
	end
	
	if(self.player_already_registerd == 0)then
		return
	end
	
	if(tonumber(self.reject_invite_round_id) == tonumber(current_round_id))then
		return
	end
	
	local mainui = getUI(self.main_window_name)
	
	Ryzhide:check_local_player_name()
	
	debug("time_to_start"..time_to_start)
	debug("most_wanted_name"..most_wanted_name)
	
	self.round_time_to_start = time_to_start
	
	if(self.player_name == most_wanted_name)then
		self.player_type = "most_wanted"
		debug("You must hide!")
		Ryzhide:build_most_wanted_window()
	else
		self.player_type = "hunter"
		debug("You are hunter wait for most wanted to hide!")
		Ryzhide:build_hunter_window()
	end
	
	addOnDbChange(mainui, self.timer_str, "Ryzhide:pars_json_data_wait_for_most_wanted()")
end

function Ryzhide:wait_most_wanted_timer_stopped()
	local mainui = getUI(self.main_window_name)
	removeOnDbChange(mainui, self.timer_str)
	
	Ryzhide:build_wait_start_window()
	
	if(self.player_type == "most_wanted")then
		if(self.spawn_a_object == 2)then
			debug("mostwanted_must_hidden_now_start")
		else
			debug("error_bad_position")
		end
	end
end

function Ryzhide:pars_json_data_wait_for_most_wanted()
	local mainui = getUI(self.main_window_name)

	if(self.round_time_to_start ~= 0)then
		local remaining_time = self.round_time_to_start - os.time()
			
		if(remaining_time < 0)then
			Ryzhide:wait_most_wanted_timer_stopped()
		else
			Ryzhide:update_timer("hide_n_hype_timer_hunter",remaining_time, (3*60))
		end
	else
		Ryzhide:wait_most_wanted_timer_stopped()
	end
	
	if(self.player_type == "most_wanted")then
		if(self.spawn_a_object == 0)then
			local regist_hunter_text = mainui:find("ready_most_wanted_text")
			regist_hunter_text:find("text").hardtext = Ryzhide:load_translation("hide_n_hype_not_ready")
			regist_hunter_text.w = "250"
			regist_hunter_text:find("text").color = "255 0 0 255"
				
			local regist_hunter_img = mainui:find("display_most_wanted_status")
			regist_hunter_img:find("img1").texture = "w_answer_16_cancel.tga"
			regist_hunter_img:find("ctrl").tooltip = Ryzhide:load_translation("hide_n_hype_not_ready")
		end
		
		if(self.spawn_a_object == 1)then
			local regist_hunter_text = mainui:find("ready_most_wanted_text")
			regist_hunter_text:find("text").hardtext = Ryzhide:load_translation("hide_n_hype_ready_wait_for_countdown")
			regist_hunter_text.w = "250"
			regist_hunter_text:find("text").color = "0 255 0 255"
			
			local regist_hunter_img = mainui:find("display_most_wanted_status")
			regist_hunter_img:find("img1").texture = "rap_invited_no_dm.tga"
			regist_hunter_img:find("ctrl").tooltip = Ryzhide:load_translation("hide_n_hype_ready_wait_for_countdown")
			
			self.spawn_a_object = 2
			self.save_x_pos,self.save_y_pos,self.save_z_pos = getPlayerPos()
		end
		
		if(self.spawn_a_object == 2)then
			local x,y,z = getPlayerPos()
			local current_distance = Ryzhide:distance_calc(x, y, z, self.save_x_pos, self.save_y_pos, self.save_z_pos)
			if(current_distance > 1)then
				debug("you_moved!!!!")
				
				Ryzhide:despawn_object(self.save_x_pos, self.save_y_pos)
	
				local regist_hunter_text = mainui:find("ready_most_wanted_text")
				regist_hunter_text:find("text").hardtext = Ryzhide:load_translation("hide_n_hype_not_ready")
				regist_hunter_text.w = "250"
				regist_hunter_text:find("text").color = "255 0 0 255"
				
				local regist_hunter_img = mainui:find("display_most_wanted_status")
				regist_hunter_img:find("img1").texture = "w_answer_16_cancel.tga"
				regist_hunter_img:find("ctrl").tooltip = Ryzhide:load_translation("hide_n_hype_not_ready")
				
				self.spawn_a_object = 0
			end
		end
	end
end

function Ryzhide:build_hunter_window()
	local window_height = 150
	local window_width = 365
	local table_width = window_width - 15

	local html_hunter=""
	html_hunter=[[<title>]]..Ryzhide:load_translation("hide_n_hype_close_your_eyes_and_count_window")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="center" width="40"><img src="ico_time.png" width="40"></td>
				<td align="center"><div id="hide_n_hype_timer_hunter" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_hunter;timer:03:00;'></div></td>
				<td align="center" width="40"><img src="ico_time.png" width="40"></td>
			</tr>
			
			<tr>
				<td colspan="3" align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_wait_for_most_wanted")..[[</h2></td>
			</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_hunter)
end

function Ryzhide:build_most_wanted_window()
	local window_height = 450
	local window_width = 415
	local table_width = window_width - 15

	local html_most_wanted=""
	html_most_wanted=[[<title>]]..Ryzhide:load_translation("hide_n_hype_go_and_hide_fast")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="center" width="40"><img src="ico_time.png" width="40"></td>
				<td align="center" width="180"><div id="hide_n_hype_timer_hunter" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_hunter;timer:03:00;'></div></td>
				<td align="center" width="40"><img src="ico_time.png" width="40"></td>
			</tr>
			
			<tr>
				<td align="center" colspan="3"><h2>]]..Ryzhide:load_translation("hide_n_hype_follow_this_rules")..[[</h2></td>
			</tr>
			
			<tr>
				<td align="center" colspan="3"><h3>1. ]]..Ryzhide:load_translation("hide_n_hype_find_a_good_hiding_spot")..[[</h3></td>
			</tr>
			<tr>
				<td align="center" colspan="3"><h3>2. ]]..Ryzhide:load_translation("hide_n_hype_used_a_object_to_hide")..[[</h3></td>
			</tr>
			<tr>
				<td align="center" colspan="3"><h3>3. ]]..Ryzhide:load_translation("hide_n_hype_wait_of_start_of_the_round")..[[</h3></td>
			</tr>
			
			<tr>
				<td colspan="3">&nbsp;</td>
			</tr>
			
			<tr>
				<td  align="center" colspan="3">]]..Ryzhide:load_translation("hide_n_hype_avaible_objects")..[[</td>
			</tr>
			
			<tr>
				<td align="center" colspan="3">
					<table width="]]..table_width..[[" border=1>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_totem_kitin_sel_nocollision')">]]..Ryzhide:load_translation("object_totem_kitin_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_tent_sel_nocollision')">]]..Ryzhide:load_translation("object_tent_sel_nocollision")..[[</a></td>
						</tr>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_spot_goo_sel_nocollision')">]]..Ryzhide:load_translation("object_spot_goo_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_tent_zorai_sel_nocollision')">]]..Ryzhide:load_translation("object_tent_zorai_sel_nocollision")..[[</a></td>
						</tr>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_giant_skull_b_lokness_c_sel_nocollision')">]]..Ryzhide:load_translation("object_giant_skull_b_lokness_c_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_tent_tryker_sel_nocollision')">]]..Ryzhide:load_translation("object_tent_tryker_sel_nocollision")..[[</a></td>
						</tr>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_1_crate_sel_nocollision')">]]..Ryzhide:load_translation("object_1_crate_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_tent_matis_sel_nocollision')">]]..Ryzhide:load_translation("object_tent_matis_sel_nocollision")..[[</a></td>
						</tr>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_1_barrel_sel_nocollision')">]]..Ryzhide:load_translation("object_1_barrel_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_tent_fyros_sel_nocollision')">]]..Ryzhide:load_translation("object_tent_fyros_sel_nocollision")..[[</a></td>
						</tr>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_fo_s2_bigroot_c_sel_nocollision')">]]..Ryzhide:load_translation("object_fo_s2_bigroot_c_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_ju_s3_dead_tree_sel_nocollision')">]]..Ryzhide:load_translation("object_ju_s3_dead_tree_sel_nocollision")..[[</a></td>
						</tr>
						<tr>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_fy_s1_burnedtree_d_sel_nocollision')">]]..Ryzhide:load_translation("object_fy_s1_burnedtree_d_sel_nocollision")..[[</a></td>
							<td align="center"><a href="ah:lua&Ryzhide:spawn_object('object_tr_s2_lokness_c_sel_nocollision')">]]..Ryzhide:load_translation("object_tr_s2_lokness_c_sel_nocollision")..[[</a></td>
						</tr>
					</table>
				</td>
			</tr>
			
			<tr>
				<td colspan="2" align="center"><div id="ready_most_wanted_text" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:ready_most_wanted_text;font_size:16;text_color:255 0 0 255;hardtext:]]..Ryzhide:load_translation("hide_n_hype_not_ready")..[[;w:250;'></div></td>
				<td align="center"><div id="display_most_wanted_status" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_only_images;id:display_most_wanted_status;img1:w_answer_16_cancel.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_not_ready")..[[;w:46;h:46;'></div></td>
			</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted)
end

function Ryzhide:spawn_object(sheet_id)
	webig:openUrlInBg("https://app.ryzom.com/app_arcc/index.php?action=mScript_Run&script=12811&command=reset_all&sheet_to_spawn="..sheet_id)
end

function Ryzhide:despawn_object(old_x, old_y)
	webig:openUrlInBg("https://app.ryzom.com/app_arcc/index.php?action=mScript_Run&script=12835&command=reset_all&play_pos_x="..old_x.."&play_pos_y="..old_y)
end


--###################################### END build and load prepartion window #######################################




--###################################### START build and load game_running window #######################################


function Ryzhide:start_the_hide_now(time_to_end, most_wanted_name, current_round_id)
	if(self.game_is_full_loaded == 0)then
		debug("game_not_loaded -- start_the_hide_now")
		return
	end
	
	if(self.player_already_registerd == 0)then
		return
	end
	
	if(tonumber(self.reject_invite_round_id) == tonumber(current_round_id))then
		return
	end
	
	local mainui = getUI(self.main_window_name)

	self.current_round_end = 0
	self.timer_hint_1 = 0
	self.timer_hint_2 = 0
	self.timer_hint_3 = 0
	self.target_name_old = "unlocked"
	
	--reset vars for game is over
	self.closed_most_wanted_not_found_hunter = 0
	self.closed_loos_hunter = 0

	debug("time_to_end: "..time_to_end)
	debug("most_wanted_name: "..most_wanted_name)
	
	Ryzhide:check_local_player_name()
	
	self.manuell_action = 0
	self.need_json_update = 1
	self.current_round_end = time_to_end
	
	if(self.player_name == most_wanted_name)then
		player_type = "most_wanted"
		debug("You must hide!")
		Ryzhide:build_game_running_most_wanted_window()
		addOnDbChange(mainui, self.timer_str, "Ryzhide:pars_json_data_game_running_most_wanted()")
	else
		player_type = "hunter"
		debug("You are hunter , hunt the most wanted!")
		Ryzhide:build_game_running_hunter_window()
		addOnDbChange(mainui, self.timer_str, "Ryzhide:pars_json_data_game_running_hunter()")
	end
end

function Ryzhide:game_running_timer_stopped()
	removeOnDbChange(getUI(self.main_window_name),self.timer_str)
	
	debug("Timer_is_over_and_game_over")
	self.need_json_update = 1
	self.json_data_ready = 0
	
	Ryzhide:build_wait_start_window()
end

function Ryzhide:display_game_running_sucess(sucess_msg)
	self.json_data_ready = 0

	Ryzhide:display_message_to_player('info', 'AMB', sucess_msg)
end

function Ryzhide:display_game_running_error(error_msg)
	self.json_data_ready = 0
	
	Ryzhide:display_message_to_player('error', 'AMB', error_msg)
end

function Ryzhide:pars_json_data_game_running_most_wanted()
		self.save_x_pos,self.save_y_pos,self.save_z_pos = getPlayerPos()

		if(self.need_json_update == 1)then
			self.need_json_update = 0
			Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=heartbeat&x='..self.save_x_pos..'&y='..self.save_y_pos..'&z='..self.save_z_pos)
		end
		
		if(self.current_round_end ~= 0)then
			local remaining_time = self.current_round_end - os.time()
			
			if(remaining_time < 0)then
				Ryzhide:game_running_timer_stopped()
			else
				Ryzhide:update_timer("hide_n_hype_timer_game_running_most_wanted",remaining_time, (3*60))
			end
		end

		if(self.json_data_ready == 1)then
			if (self.hide_n_hide_json_data) then
				if (self.hide_n_hide_json_data.error) then
					if(self.hide_n_hide_json_data.error == "not_most_wanted")then
						Ryzhide:display_game_running_error(Ryzhide:load_translation("hide_n_hype_not_most_wanted"))
					end
				elseif (self.hide_n_hide_json_data.success) then
					if(self.hide_n_hide_json_data.success == "ok_most_wanted_heartbeat")then
						--Ryzhide:display_game_running_sucess("sucess_heartbeat: "..self.hide_n_hide_json_data.success)
					end
				else
					Ryzhide:display_game_running_error("no 'error' or 'success' found")
				end
			else
				Ryzhide:display_game_running_error("JSON-Decode failed")
			end
			
			self.need_json_update = 1
		end
end

function Ryzhide:update_hint_data()
	debug("hint_1: "..self.hide_n_hide_json_data.hint_1)
	debug("hint_2: "..self.hide_n_hide_json_data.hint_2)
	debug("hint_3: "..self.hide_n_hide_json_data.hint_3)
	
	self.hint_1_text = self.hide_n_hide_json_data.hint_1
	self.hint_2_text = self.hide_n_hide_json_data.hint_2
	self.hint_3_text = self.hide_n_hide_json_data.hint_3
end

function Ryzhide:check_player_target()
	local mainui = getUI(self.main_window_name)
	local check_found_most_wanted_button = mainui:find("check_found_most_wanted")
	
	local hnh_target_x_pos = 0
    local hnh_target_y_pos = 0
    local hnh_target_z_pos = 0
	
	local hnh_player_x_pos = 0
	local hnh_player_y_pos = 0
	local hnh_player_z_pos = 0
	
	
	if(isTargetNPC() == false and isTargetPlayer() == false and isTargetUser() == false)then
	    hnh_player_x_pos,hnh_player_y_pos,hnh_player_z_pos = getPlayerPos()
	
    	if(getTargetPos() ~= nil)then
    		hnh_target_x_pos,hnh_target_y_pos,hnh_target_z_pos = getTargetPos()
    	end
	
		local current_distance = Ryzhide:distance_calc(hnh_target_x_pos, hnh_target_y_pos, hnh_target_z_pos, hnh_player_x_pos, hnh_player_y_pos, hnh_player_z_pos)
		if(current_distance <= 5)then
			
			if(self.target_name_old == "locked")then
				local old_pos_distance = Ryzhide:distance_calc(hnh_player_x_pos, hnh_player_y_pos, hnh_player_z_pos, self.save_x_pos, self.save_y_pos, self.save_z_pos)
				if(old_pos_distance > 1)then
					debug("Reset Butto now!!!!!!!!")
					self.target_name_old = "unlocked"
					self.save_x_pos,self.save_y_pos,self.save_z_pos = getPlayerPos()
					check_found_most_wanted_button:find("img1").texture = ""
					check_found_most_wanted_button:find("ctrl").active = true
				end
			end
			
			if(self.target_name_old == "unlocked" and check_found_most_wanted_button:find("img1").texture ~= "curs_rotate.tga")then
				--unlock the check_found button
				debug("texture: "..check_found_most_wanted_button:find("img1").texture)
				debug("unlocked")
				check_found_most_wanted_button:find("back").texture = "r2_icon_animation_target_green.png"
				check_found_most_wanted_button:find("ctrl").active = true
			end
		else
			--disable the check_found button
			check_found_most_wanted_button:find("back").texture = "r2_icon_animation_target.png"
			check_found_most_wanted_button:find("ctrl").active = false
		end
	else
		--disable the check_found button
		check_found_most_wanted_button:find("back").texture = "r2_icon_animation_target.png"
		check_found_most_wanted_button:find("ctrl").active = false
	end
end

function Ryzhide:display_try_found_sucess(sucess_msg)
	self.json_data_ready = 0

	Ryzhide:display_message_to_player('info', 'AMB', sucess_msg)
end

function Ryzhide:display_try_found_error(error_msg)
	self.json_data_ready = 0
	
	Ryzhide:display_message_to_player('error', 'AMB', error_msg)
end

function Ryzhide:feedback_found_button(feedback)
	local mainui = getUI(self.main_window_name)
	local mainui_group = getUI(self.main_window_name):find("check_found_most_wanted")
	
	if(feedback == "success")then
		mainui_group:find("img1").texture = "rap_invited_no_dm.png"
		mainui_group:find("ctrl").active = false
		
		local mainui = getUI(self.main_window_name)
		removeOnDbChange(mainui, self.timer_str)
		
		Ryzhide:build_wait_finish_window()
	else
		self.save_x_pos,self.save_y_pos,self.save_z_pos = getPlayerPos()
		self.target_name_old = "locked"
		mainui_group:find("img1").texture = "w_answer_16_cancel.png"
		mainui_group:find("ctrl").active = false
	end
end

function Ryzhide:pars_json_data_game_running_hunter()
	if(self.need_json_update == 1 and self.manuell_action == 0)then
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=jsonData&need_json_data=running_game')
		Ryzhide:unlock_hints()
	end
	
	if(self.need_json_update == 1 and self.manuell_action == 1)then
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=check_found')
	end
	
	Ryzhide:check_player_target()
	
	if(self.json_data_ready == 1)then
			if (self.hide_n_hide_json_data) then
				if (self.hide_n_hide_json_data.error) then
					debug("error: "..self.hide_n_hide_json_data.error.." m: "..self.manuell_action)
					if(self.manuell_action == 1)then
						Ryzhide:display_try_found_error(Ryzhide:load_translation(self.hide_n_hide_json_data.error))
						if(self.hide_n_hide_json_data.error == "not_near_most_wanted")then
							Ryzhide:feedback_found_button("error")
							self.manuell_action = 0
						end
					end
				elseif (self.hide_n_hide_json_data.success) then
					debug("success: "..self.hide_n_hide_json_data.success.." m: "..self.manuell_action)
					if(self.manuell_action == 1)then
						if(self.hide_n_hide_json_data.success == "ok_you_found_most_wanted")then
							if(self.hide_n_hide_json_data.success == "ok_you_found_most_wanted")then
								local sucess_message = "hide_n_hype_ok_you_found_most_wanted"
							end
							Ryzhide:display_try_found_sucess(Ryzhide:load_translation(sucess_message))
							Ryzhide:feedback_found_button("success")
							self.manuell_action = 0
						end
					else
						Ryzhide:update_hint_data()
					end
					
				else
					debug("no 'error' or 'success' found")
				end
			else
				debug("JSON-Decode failed")
			end
			
			self.need_json_update = 1
		end


	if(self.current_round_end ~= 0)then
		local remaining_time = self.current_round_end - os.time()
		local remaining_time_hint_1 = (self.current_round_end - os.time()) - 180
		local remaining_time_hint_2 = (self.current_round_end - os.time()) - 120
		local remaining_time_hint_3 = (self.current_round_end - os.time()) - 60
		
		if(remaining_time < 0)then
			Ryzhide:game_running_timer_stopped()
		else
			if(remaining_time_hint_1 >= 0 and self.timer_hint_1 == 0)then
				Ryzhide:update_timer("hide_n_hype_timer_game_running_hint",remaining_time_hint_1, (0.5*60))
			elseif(remaining_time_hint_1 < 0)then
				if(self.timer_hint_1 == 0)then
					self.timer_hint_1 = 1
					debug("self.timer_hint_1")
				end
			end
			
			if(remaining_time_hint_2 >= 0 and self.timer_hint_2 == 0 and self.timer_hint_1 == 1)then
				Ryzhide:update_timer("hide_n_hype_timer_game_running_hint",remaining_time_hint_2, (0.5*60))
			elseif(remaining_time_hint_2 < 0)then
				if(self.timer_hint_2 == 0)then
					self.timer_hint_2 = 1
					debug("self.timer_hint_2")
				end
			end
			
			if(remaining_time_hint_3 >= 0 and self.timer_hint_3 == 0 and self.timer_hint_1 == 1 and self.timer_hint_2 == 1)then
				Ryzhide:update_timer("hide_n_hype_timer_game_running_hint",remaining_time_hint_3, (0.5*60))
			elseif(remaining_time_hint_3 < 0)then
				if(self.timer_hint_3 == 0)then
					self.timer_hint_3 = 1
					debug("self.timer_hint_3")
				end
			end
			
			Ryzhide:update_timer("hide_n_hype_timer_game_running_hunter",remaining_time, (3*60))
		end
	end
end

function Ryzhide:unlock_hints()
	local mainui = getUI(self.main_window_name)
	
	if(self.timer_hint_1 == 1)then
		local hint_1_text_controll = mainui:find("hint_1")
		debug("set_hint_1: "..self.hint_1_text)
		hint_1_text_controll:find("text").hardtext = self.hint_1_text
		hint_1_text_controll:find("text").color = "0 255 0 255"
	end
	if(self.timer_hint_2 == 1)then
		local hint_2_text_controll = mainui:find("hint_2")
		debug("set_hint_2: "..self.hint_2_text)
		hint_2_text_controll:find("text").hardtext = self.hint_2_text
		hint_2_text_controll:find("text").color = "0 255 0 255"
	end
	if(self.timer_hint_3 == 1)then
		local hint_3_text_controll = mainui:find("hint_3")
		debug("set_hint_3: "..self.hint_3_text)
		hint_3_text_controll:find("text").hardtext = self.hint_3_text
		hint_3_text_controll:find("text").color = "0 255 0 255"
	end
end

function Ryzhide:click_icon_found_most_wanted(id)
	local mainui = getUI(self.main_window_name)
	
	if(id == "check_found_most_wanted")then
		self.manuell_action = 1
		self.need_json_update = 0
		Ryzhide:start_fetch_json_data('https://app.ryzom.com/app_ryzhide/?mode=check_found')
	end
	
	local mainui_group = getUI(self.main_window_name):find(id)
	local image_ui_controll = mainui_group:find("img1").texture
	if(image_ui_controll == "")then
		mainui_group:find("img1").texture = "curs_rotate.png"
		mainui_group:find("ctrl").active = false
	end
end

function Ryzhide:build_game_running_most_wanted_window()
	local window_height = 120
	local window_width = 365
	local table_width = window_width - 15

	local html_running_most_wanted=""
	html_running_most_wanted=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_running_window")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="center"><div id="hide_n_hype_timer_game_running_most_wanted" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_game_running_most_wanted;timer:15:00;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_keep_calm")..[[</h2></td>
			</tr>
			</table>]]
	Ryzhide:open_resize_main_window(window_height, window_width, html_running_most_wanted)
end

function Ryzhide:build_game_running_hunter_window()
	local window_height = 280
	local window_width = 365
	local table_width = window_width - 15

	local html_running_hunter=""
	html_running_hunter=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_running_window")..[[</title><br>
		   <table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="center" colspan="2"><div id="hide_n_hype_timer_game_running_hunter" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_game_running_hunter;timer:15:00;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_new_hint_in")..[[</h2></td>
				<td align="center"><div id="hide_n_hype_timer_game_running_hint" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_game_running_hint;timer:05:00;'></td>
			</tr>
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_hint_1")..[[</h2></td>
				<td align="center"><div id="hint_1" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hint_1;font_size:16;text_color:255 0 0 255;hardtext:????;w:250;'></div></td>
			</tr>
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_hint_2")..[[</h2></td>
				<td align="center"><div id="hint_2" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hint_2;font_size:16;text_color:255 0 0 255;hardtext:????;w:250;'></div></td>
			</tr>
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_hint_3")..[[</h2></td>
				<td align="center"><div id="hint_3" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_text;id:hint_3;font_size:16;text_color:255 0 0 255;hardtext:????;w:250;'></div></td>
			</tr>
			
			<tr>
				<td align="center" colspan="2"><div id="check_found_most_wanted" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:check_found_most_wanted;bg:r2_icon_animation_target.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_check_found_most_wanted")..[[;lua_function:click_icon_found_most_wanted;'></div></td>
			</tr>
			</table>]]
	Ryzhide:open_resize_main_window(window_height, window_width, html_running_hunter)
end

function Ryzhide:build_wait_finish_window()
	local window_height = 45
	local window_width = 380
	local table_width = window_width - 15

	local html_wait_finish_window=""
	html_wait_finish_window=[[<title></title>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td><h1>]]..Ryzhide:load_translation("hide_n_hype_wait_for_game_finish")..[[</h1></td>
			</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_wait_finish_window)
end


--###################################### END build and load game_running window #######################################




--###################################### START build and load claim_your_price window #######################################


function Ryzhide:game_over_most_wanted_not_found(most_wanted_name,current_round_id,timer_claim_reward)
	if(self.game_is_full_loaded == 0)then
		debug("game_not_loaded -- game_over_most_wanted_not_found")
		return
	end
	
	if(self.player_already_registerd == 0)then
		return
	end
	
	
	if(tonumber(self.reject_invite_round_id) == tonumber(current_round_id))then
		return
	end
	
	local mainui = getUI(self.main_window_name)
	removeOnDbChange(mainui,self.timer_str)
	
	debug("game_over_most_wanted_not_found: "..timer_claim_reward)
	debug("most_wanted: "..most_wanted_name)
	
	if(most_wanted_name == "")then
		debug("error_most_wanted_name_empty")
		Ryzhide:close_window(self.main_window_name, "")
		return
	end
	
	Ryzhide:check_local_player_name()
	
	if(self.player_name == most_wanted_name)then
		if(self.reward_already_claimed == 0)then
			Ryzhide:build_most_wanted_not_found_most_wanted()
			addOnDbChange(mainui, self.timer_str, "Ryzhide:timer_to_claim_rewards("..timer_claim_reward..")")
		end
	else
		if(self.closed_most_wanted_not_found_hunter == 0)then
			Ryzhide:build_most_wanted_not_found_hunter(most_wanted_name)
		end
	end
end

function Ryzhide:game_over_most_wanted_found(hunter_name,most_wanted_name,current_round_id,timer_claim_reward)
	if(self.game_is_full_loaded == 0)then
		debug("game_not_loaded -- game_over_most_wanted_found")
		return
	end
	if(tonumber(self.reject_invite_round_id) == tonumber(current_round_id))then
		return
	end
	
	debug("game_over_most_wanted_found: "..timer_claim_reward)
	debug("hunter: "..hunter_name)
	debug("most_wanted: "..most_wanted_name)
	local mainui = getUI(self.main_window_name)
	removeOnDbChange(mainui,self.timer_str)
	
	Ryzhide:check_local_player_name()
	
	if(self.player_name == most_wanted_name)then
		if(self.reward_already_claimed == 0)then
			self.player_type = "most_wanted"
			debug("You loose! Found by "..hunter_name)
			Ryzhide:build_finished_most_wanted(hunter_name,most_wanted_name)
			addOnDbChange(mainui, self.timer_str, "Ryzhide:timer_to_claim_rewards("..timer_claim_reward..")")
		end
	elseif(self.player_name == hunter_name)then
		if(self.reward_already_claimed == 0)then
			self.player_type = "hunter_found"
			debug("You win!")
			Ryzhide:build_finished_hunter(hunter_name,most_wanted_name)
			addOnDbChange(mainui, self.timer_str, "Ryzhide:timer_to_claim_rewards("..timer_claim_reward..")")
		end
	else
		if(self.closed_loos_hunter == 0)then
			self.player_type = "hunter"
			debug("You loose:"..hunter_name.." was faster then you")
			Ryzhide:build_loos_hunter(hunter_name,most_wanted_name)
		end
	end
end

function Ryzhide:timer_to_claim_rewards(timer_claim_reward)
	if(timer_claim_reward ~= 0)then
		local remaining_time = timer_claim_reward - os.time()
			
		if(remaining_time < 0)then
			Ryzhide:game_running_timer_stopped()
		else
			Ryzhide:update_timer("hide_n_hype_timer_claim_reward",remaining_time, (3*60))
		end
	end
end

function Ryzhide:game_running_timer_stopped()
	removeOnDbChange(getUI(self.main_window_name),self.timer_str)
	
	debug("Timer_is_over_no_rewards")
	self.need_json_update = 1
	self.json_data_ready = 0
	
	Ryzhide:close_window(self.main_window_name, "")
end

function Ryzhide:open_clain_reward_script()
	webig:openUrlInBg("https://app.ryzom.com/app_arcc/index.php?action=mScript_Run&script=12815&command=reset_all")
end

function Ryzhide:build_finished_hunter(hunter_name,most_wanted_name)
	local window_height = 300
	local window_width = 365
	local table_width = window_width - 15

	local html_finished_hunter=""
	html_finished_hunter=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_over_window")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
				<tr>
					<td align="center"><div id="hide_n_hype_timer_claim_reward" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_claim_reward;timer:03:00;'></div></td>
				</tr>
				
				<tr>
					<td align="center"><img src="ico_happy_winner.png" width="80" height="80" ></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_you_found_the_most_wanted")..[[</h2></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..most_wanted_name..[[</h2></td>
				</tr>
				
				<tr>
					<td>&nbsp;</td>
				</tr>
				
				<tr>
					<td align="center"><a href="ah:lua&Ryzhide:open_clain_reward_script()"><h2>]]..Ryzhide:load_translation("hide_n_hype_click_here_to_claim_your_price")..[[</h2></a></td>
				</tr>
			</table>]]
	Ryzhide:open_resize_main_window(window_height, window_width, html_finished_hunter)
end

function Ryzhide:build_finished_most_wanted(hunter_name,most_wanted_name)
	local window_height = 320
	local window_width = 380
	local table_width = window_width - 15

	local html_finished_most_wanted=""
	html_finished_most_wanted=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_over_window")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
				<tr>
					<td align="center"><div id="hide_n_hype_timer_claim_reward" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_claim_reward;timer:03:00;'></div></td>
				</tr>
				
				<tr>
					<td align="center"><img src="icon_unhappy.png" width="80" height="80" ></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_time_to_come_out")..[[</h2></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..hunter_name..[[</h2></td>
				</tr>
				
				<tr >
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_found_you")..[[</h2></td>
				</tr>
				
				<tr>
					<td>&nbsp;</td>
				</tr>
				
				<tr>
					<td align="center"><a href="ah:lua&Ryzhide:open_clain_reward_script()"><h2>]]..Ryzhide:load_translation("hide_n_hype_click_here_to_claim_your_price")..[[</h2></a></td>
				</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_finished_most_wanted)
end

function Ryzhide:build_loos_hunter(hunter_name,most_wanted_name)
	local window_height = 250
	local window_width = 380
	local table_width = window_width - 15

	local html_loos_hunter=""
	html_loos_hunter=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_over_window")..[[</title>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
				<tr>
					<td align="right"><div id="close_window_build_loos_hunter" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_build_loos_hunter;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
				</tr>
				
				<tr>
					<td align="center"><img src="icon_unhappy.png" width="80" height="80" ></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_better_luck_next_time")..[[</h2></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..hunter_name..[[</h2></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_has_out_searched_you")..[[</h2></td>
				</tr>
				
				<tr>
					<td>&nbsp;</td>
				</tr>
				
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_loos_hunter)
end

function Ryzhide:build_most_wanted_not_found_hunter(most_wanted_name)
	local window_height = 200
	local window_width = 370
	local table_width = window_width - 15

	local html_most_wanted_not_found_hunter=""
	html_most_wanted_not_found_hunter=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_over_window")..[[</title>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
				<tr>
					<td align="right"><div id="close_window_build_most_wanted_not_found_hunter" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_build_most_wanted_not_found_hunter;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
				</tr>
			
				<tr>
					<td align="center"><img src="icon_unhappy.png" width="80" height="80" ></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_better_luck_next_time")..[[</h2></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..most_wanted_name..[[</h2></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_keep_untouched_and_not_found")..[[</h2></td>
				</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted_not_found_hunter)
end

function Ryzhide:build_most_wanted_not_found_most_wanted()
	local window_height = 300
	local window_width = 365
	local table_width = window_width - 15

	local html_most_wanted_not_found_most_wanted=""
	html_most_wanted_not_found_most_wanted=[[<title>]]..Ryzhide:load_translation("hide_n_hype_game_is_over_window")..[[</title><br>
			<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
				<tr>
					<td align="center"><div id="hide_n_hype_timer_claim_reward" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_claim_reward;timer:03:00;'></div></td>
				</tr>
				
				<tr>
					<td align="center"><img src="ico_happy_winner.png" width="80" height="80" ></td>
				</tr>
				
				<tr>
					<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_you_won_no_player_found_you")..[[</h2></td>
				</tr>
				
				<tr>
					<td>&nbsp;</td>
				</tr>
				
				<tr>
					<td align="center"><a href="ah:lua&Ryzhide:open_clain_reward_script()"><h2>]]..Ryzhide:load_translation("hide_n_hype_click_here_to_claim_your_price")..[[</h2></a></td>
				</tr>
			</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted_not_found_most_wanted)
end


--###################################### END build and load claim_your_price window #######################################



--###################################### Start error handling #######################################

function Ryzhide:build_most_wanted_bad_position()
	local window_height = 140
	local window_width = 370
	local table_width = window_width - 15

	local html_most_wanted_bad_position = ""
	html_most_wanted_bad_position=[[<title>]]..Ryzhide:load_translation("hide_n_hype_abort_most_wanted_bad_position_window")..[[</title>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="right"><div id="close_window_abort_most_wanted_bad_position_window" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_abort_most_wanted_bad_position_window;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><img src="icon_dead.png" width="80" height="80" ></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_abort_most_wanted_bad_position")..[[</h2></td>
			</tr>
		</table>]]
		Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted_bad_position)
end

function Ryzhide:build_error_not_enough_players()
	local window_height = 140
	local window_width = 370
	local table_width = window_width - 15

	local html_error_not_enough_players = ""
	html_error_not_enough_players=[[<title>]]..Ryzhide:load_translation("hide_n_hype_abort_not_enough_player_to_start_a_round_window")..[[</title>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="right"><div id="close_window_abort_not_enough_player_to_start_a_round_window" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_abort_not_enough_player_to_start_a_round_window;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><img src="icon_dead.png" width="80" height="80" ></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_abort_not_enough_player")..[[</h2></td>
			</tr>
		</table>]]
		Ryzhide:open_resize_main_window(window_height, window_width, html_error_not_enough_players)
end

function Ryzhide:build_most_wanted_offline_window()
	local window_height = 140
	local window_width = 365
	local table_width = window_width - 15

	local html_most_wanted_offline_window=""
	html_most_wanted_offline_window=[[<title>]]..Ryzhide:load_translation("hide_n_hype_wait_for_most_wanted_come_back_online_window")..[[</title><br>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr align="center">
				<td align="center"><img src="ico_time.png" width="40"></td>
				<td align="center"><div id="hide_n_hype_timer_most_wanted_offline" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_timer;id:hide_n_hype_timer_most_wanted_offline;timer:02:00;'></div></td>
				<td align="center"><img src="ico_time.png" width="40"></td>
			</tr>
			
			<tr>
				<td colspan="3" align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_most_wanted_lost_connection")..[[</h2></td>
			</tr>
		</table>]]
			
	Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted_offline_window)
end

function Ryzhide:build_most_wanted_offline()
	local window_height = 140
	local window_width = 370
	local table_width = window_width - 15

	local html_most_wanted_offline = ""
	html_most_wanted_offline=[[<title>]]..Ryzhide:load_translation("hide_n_hype_abort_most_wanted_offline_game_abort_window")..[[</title>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="right"><div id="close_window_abort_most_wanted_offline_game_abort_window" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_abort_most_wanted_offline_game_abort_window;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><img src="icon_dead.png" width="80" height="80" ></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_most_wanted_offline_game_abort")..[[</h2></td>
			</tr>
		</table>]]
		Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted_offline)
end

function Ryzhide:build_most_wanted_moved()
	local window_height = 140
	local window_width = 370
	local table_width = window_width - 15

	local html_most_wanted_moved = ""
	html_most_wanted_moved=[[<title>]]..Ryzhide:load_translation("hide_n_hype_abort_most_wanted_moved_window")..[[</title>
		<table width="]]..table_width..[[" cellpadding="2" cellspacing="0" border=1>
			<tr>
				<td align="right"><div id="close_window_abort_most_wanted_moved_window" class='ryzom-ui-grouptemplate' style='display:inline-block;template:hide_n_hype_button_images;id:close_window_abort_most_wanted_moved_window;bg:w_win_close.png;tooltip:]]..Ryzhide:load_translation("hide_n_hype_click_to_close")..[[;lua_function:click_close_button;w:16;h:16;'></div></td>
			</tr>
			
			<tr>
				<td align="center"><img src="icon_dead.png" width="80" height="80" ></td>
			</tr>
			
			<tr>
				<td align="center"><h2>]]..Ryzhide:load_translation("hide_n_hype_most_wanted_moved_game_abort")..[[</h2></td>
			</tr>
		</table>]]
		Ryzhide:open_resize_main_window(window_height, window_width, html_most_wanted_moved)
end

function Ryzhide:abort_because_of(process_info_name,round_id)
	if(self.game_is_full_loaded == 0)then
		debug("game_not_loaded -- abort_because_of: "..process_info_name)
		return
	end
	
	if(self.player_already_registerd == 0)then
		return
	end
	
	
	local mainui = getUI(self.main_window_name)
	removeOnDbChange(mainui, self.timer_str)

	debug("reason: "..process_info_name.." round_id: "..round_id)
	debug("self: "..self.reject_invite_round_id.." round: "..round_id)
	
	if(tonumber(self.reject_invite_round_id) == tonumber(round_id))then
		return
	end
	
	if(process_info_name == "abort_not_enough_player")then
		Ryzhide:build_error_not_enough_players()
	end
	
	if(process_info_name == "most_wanted_moved")then
		Ryzhide:build_most_wanted_moved()
	end
	
	
	if(process_info_name == "most_wanted_bad_position")then
		Ryzhide:build_most_wanted_bad_position()
	end
	
	if(process_info_name == "error_no_player_founds" or process_info_name == "error_no_hunter_found" or process_info_name == "error_no_most_wanted_found")then
		Ryzhide:build_error_not_enough_players()
	end

	if(process_info_name == "most_wanted_offline")then
		Ryzhide:build_most_wanted_offline()
	end
end

function Ryzhide:most_wanted_offline(time_until_abort)
	local mainui = getUI(self.main_window_name)
	removeOnDbChange(mainui, self.timer_str)

	self.current_round_end = time_until_abort
	Ryzhide:build_most_wanted_offline_window()
	addOnDbChange(mainui, self.timer_str, "Ryzhide:run_timer_most_wanted_offline()")
end

function Ryzhide:run_timer_most_wanted_offline()
	if(self.current_round_end ~= 0)then
		local remaining_time = self.current_round_end - os.time()
			
		if(remaining_time < 0)then
			Ryzhide:game_running_timer_stopped()
		else
			Ryzhide:update_timer("hide_n_hype_timer_most_wanted_offline",remaining_time, (3*60))
		end
	end
end

--###################################### END error handling #######################################


