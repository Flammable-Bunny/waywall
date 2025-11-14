-- ===========================================
--  Animated Twitch / 7TV Chat Overlay
--  Based on Arsoniv's Non-Animated version
-- ===========================================

local waywall = require("waywall")
local priv    = require("priv")
local utf8    = require("utf8")
local json    = require("dkjson")

-- Remove invisible characters (prevents rendering glitches)
local INVISIBLE_CHARS = {
	[0x200B] = true, [0x200C] = true, [0x200D] = true,
	[0x034F] = true, [0xFE0F] = true,
}
local function strip_invisible(s)
	local t = {}
	for _, c in utf8.codes(s) do
		if not INVISIBLE_CHARS[c] then t[#t+1] = utf8.char(c) end
	end
	return table.concat(t)
end

-- === File helpers ===
local function read_raw_data(filename)
	local file = assert(io.open(filename, "rb"), "failed to open " .. filename)
	local data = file:read("*all"); file:close(); return data
end

local function read_json(filename)
	local file = assert(io.open(filename, "r"), "failed to open " .. filename)
	local data = file:read("*all"); file:close(); return json.decode(data)
end

-- === Config file paths ===
local atlas_filename    = "/home/bunny/.config/waywall/atlas.raw"
local emoteset_filename = "/home/bunny/.config/waywall/emoteset.json"

-- === Optional Twitch set, uncomment this and bottom part to download Twitch emote sets. ===
-- local twitch_atlas_filename  = "/home/bunny/.config/waywall/atlas_twitch.raw"
-- local twitch_json_filename   = "/home/bunny/.config/waywall/emoteset_twitch.json"

-- === Chat object factory ===
local function new_chat(channel, x, y, size)
	local CHAT = {
		channel          = channel,
		chat_x           = x,
		chat_y           = y,
		size             = size,
		ls               = 20,    -- line spacing (reccomended 15 minimum to prevent emote clipping)
		emote_h          = 32,    -- emote height (px)
		max_cols         = 50,    -- max chars per line
		max_lines        = 20,    -- max visible messages
		messages         = {},
		emote_set        = {},
		emote_atlas      = nil,
		emote_images     = {},
		chat_text        = nil,
		irc_client       = nil,
		has_connected    = false,
		ip               = priv.IRC_IP,
		port             = priv.IRC_PORT,
		username         = priv.IRC_USERNAME,
		token            = priv.IRC_TOKEN,
		message_lifespan = 25000, -- message lifetime (ms)
		self_id          = 0,     -- local id counter
	}

	-- Clear rendered text and emotes
	local function clear_drawables()
		if CHAT.chat_text then CHAT.chat_text:close(); CHAT.chat_text = nil end
		for _, v in ipairs(CHAT.emote_images) do v:close() end
		CHAT.emote_images = {}
	end

	-- Remove a message by ID and redraw
	local function remove_message_by_id(id)
		for i = #CHAT.messages, 1, -1 do
			if CHAT.messages[i]._id == id then
				table.remove(CHAT.messages, i)
				break
			end
		end
		CHAT.redraw()
	end

	-- Schedule message removal
	local function schedule_remove(id)
		if not CHAT.message_lifespan then return end
		waywall.sleep(CHAT.message_lifespan)
		remove_message_by_id(id)
	end

	-- Incremental ID generator
	local function next_id()
		CHAT.self_id = CHAT.self_id + 1
		return tostring(CHAT.self_id)
	end

	-- Redraw chat window
	function CHAT.redraw()
		clear_drawables()
		local text_buf, current_line = "", 0

		for _, msg in ipairs(CHAT.messages) do
			local s = string.sub(msg.text, 1, CHAT.max_cols)
			local body = ""
			local prefix = "<" .. msg.color .. "FF>" .. msg.user .. "<#FFFFFFFF>: "

			for word in s:gmatch("%S+") do
				word = strip_invisible(word)
				local e = CHAT.emote_set[word]
				if e then
					-- Calculate position for THIS LINE ONLY (prefix + body so far)
					local advance = waywall.text_advance(prefix .. body, CHAT.size)
					local emote_h = CHAT.emote_h
					local emote_w = emote_h * (e.w / e.h)
					local spacing_before = 3
					local spacing_after = 6

					local line_h = CHAT.size + CHAT.ls
					local text_baseline_y = CHAT.chat_y + CHAT.size + current_line * line_h
					-- Center emote vertically with text
					local emote_y = text_baseline_y - emote_h / 2 - CHAT.size / 2

					local img
					if e.animated then
						img = waywall.animated_image(e.path, {
							dst = { x = advance.x + CHAT.chat_x + spacing_before, y = emote_y, w = emote_w, h = emote_h },
						})
					else
						img = waywall.image_a({
							src   = { x = e.x, y = e.y, w = e.w, h = e.h },
							dst   = { x = advance.x + CHAT.chat_x + spacing_before, y = emote_y, w = emote_w, h = emote_h },
							atlas = CHAT.emote_atlas,
						})
					end
					if img then table.insert(CHAT.emote_images, img) end

					-- NOW add the spacing to push next text past the emote
					body = body .. "<+" .. (emote_w + spacing_before + spacing_after) .. ">"
				else
					body = body .. word .. " "
				end
			end

			text_buf = text_buf .. prefix .. body .. "\n"
			current_line = current_line + 1
		end

		CHAT.chat_text = waywall.text(text_buf, {
			x = CHAT.chat_x, y = CHAT.chat_y + CHAT.size, size = CHAT.size, ls = CHAT.ls,
		})
	end

	-- IRC message handler
	local function irc_callback(line)
		if not line:match("PRIVMSG") then return end

		local color  = line:match("color=([^;]+)") or "#FFFFFF"
		local user   = line:match("display%-name=([^;]+)") or "unknown"
		local text   = line:match("PRIVMSG #[^:]+:(.+)")
		local msg_id = line:match("id=([^;]+)") or next_id()

		if not text then return end

		table.insert(CHAT.messages, {
			user = user, color = color, text = text, _id = msg_id,
		})
		if #CHAT.messages > CHAT.max_lines then table.remove(CHAT.messages, 1) end
		CHAT.redraw()
		schedule_remove(msg_id)
	end

	-- Send message
	function CHAT:send(message)
		if not self.irc_client then return end
		local id = next_id()
		self.irc_client:send("PRIVMSG #" .. self.channel .. " :" .. message .. "\r\n")

		table.insert(self.messages, {
			user = self.username, color = "#1a7286",
			text = strip_invisible(message), _id = id,
		})
		if #self.messages > self.max_lines then table.remove(self.messages, 1) end
		self.redraw()
		schedule_remove(id)
	end

	-- Initialize connection + load emotes
	function CHAT:open()
		if self.has_connected then return end
		self.has_connected = true
		print("Starting Chat...")

		-- Load 7TV emoteset (primary)
		print("Loading 7TV emote atlas from:", atlas_filename)
		print("Loading 7TV emote set from:", emoteset_filename)
		self.emote_set = read_json(emoteset_filename)
		local atlas_data = read_raw_data(atlas_filename)
		self.emote_atlas = waywall.atlas(2048, atlas_data)

		local count = 0
		for _ in pairs(self.emote_set) do count = count + 1 end
		print("Loaded", count, "7TV emotes")

		-- Twitch emotes (remove the [] and --)
		--[[
		print("Loading Twitch emote atlas from:", twitch_atlas_filename)
		print("Loading Twitch emote set from:", twitch_json_filename)
		self.emote_set_twitch = read_json(twitch_json_filename)
		local twitch_atlas_data = read_raw_data(twitch_atlas_filename)
		self.emote_atlas_twitch = waywall.atlas(2048, twitch_atlas_data)
		local twitch_count = 0
		for _ in pairs(self.emote_set_twitch) do twitch_count = twitch_count + 1 end
		print("Loaded", twitch_count, "Twitch emotes")
		]]

		-- IRC connection
		self.irc_client = waywall.irc_client_create(
			self.ip, self.port, self.username, self.token, irc_callback
		)
		waywall.sleep(3000)
		self.irc_client:send("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n")
		self.irc_client:send("JOIN #" .. self.channel .. "\r\n")
	end

	return CHAT
end

return new_chat
