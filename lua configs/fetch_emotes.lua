-- ===========================================================================
--  fetch_emotes.lua
--  Waywall 7TV Emote Fetcher
--  Rewritten with full comments for clarity and correctness.
-- ===========================================================================

local waywall = require("waywall")
local json = require("dkjson")

-- ---------------------------------------------------------------------------
-- Module structure
-- ---------------------------------------------------------------------------
local M = {
	instances = {}, -- table to hold active fetch states
}

-- ---------------------------------------------------------------------------
-- Helper: write data to file
-- ---------------------------------------------------------------------------
local function write_file(filename, data)
	local file = io.open(filename, "wb")
	if not file then
		error("Failed to open file: " .. filename)
	end
	file:write(data)
	file:close()
end

-- ---------------------------------------------------------------------------
-- Main fetch entrypoint
-- ---------------------------------------------------------------------------
function M.Fetch(id)
-- Persistent state for this fetch session
	local state = {
		atlas_filename = "/home/bunny/.config/waywall/atlas.raw",
		emoteset_filename = "/home/bunny/.config/waywall/emoteset.json",
		emotes_dir = "/home/bunny/.config/waywall/emotes/",
		target_len = 0,            -- total emotes expected
		clen = 0,                  -- completed emotes
		emote_atlas = nil,
		http_emoteset = nil,
		emote_set = {},
		http_index = 1,            -- index for round-robin download clients
		http_clients = {},
		current_atlas_x = 0,
		current_atlas_y = 0,
		max_row_height = 0,
	}

	-- Ensure emotes directory exists
	os.execute("mkdir -p " .. state.emotes_dir)

	-- -----------------------------------------------------------------------
	-- Export final atlas and emote JSON
	-- -----------------------------------------------------------------------
	local function export_data()
		local atlas_data = state.emote_atlas:get_dump()
		write_file(state.atlas_filename, atlas_data)

		local emote_json = json.encode(state.emote_set, { indent = true })
		write_file(state.emoteset_filename, emote_json)

		local count = 0
		for _ in pairs(state.emote_set) do
			count = count + 1
		end
		print(string.format("Export complete! %d emotes saved", count))
	end

	-- -----------------------------------------------------------------------
	-- Process individual emote download
	-- -----------------------------------------------------------------------
	local function process_emote(data, url)
		local name = url:match("[?&]n=([^&]+)")
		local width = tonumber(url:match("[?&]w=(%d+)")) or 32
		local height = tonumber(url:match("[?&]h=(%d+)")) or 32
		local is_animated = url:match("%.avif") ~= nil

		if not name then
			return
		end

		if is_animated then
		-- Animated emote: saved individually as AVIF
			local filename = state.emotes_dir .. name .. ".avif"
			write_file(filename, data)
			state.emote_set[name] = {
				animated = true,
				path = filename,
				w = width,
				h = height,
			}
		else
		-- Static PNG emote: packed into atlas
			if state.current_atlas_x + width > 2048 then
				state.current_atlas_x = 0
				state.current_atlas_y = state.current_atlas_y + state.max_row_height
				state.max_row_height = 0

				-- Reset if atlas height exceeded
				if state.current_atlas_y + height > 2048 then
					state.current_atlas_x = 0
					state.current_atlas_y = 0
					state.max_row_height = 0
				end
			end

			-- Insert into shared atlas texture
			state.emote_atlas:insert_raw(data, state.current_atlas_x, state.current_atlas_y)
			state.emote_set[name] = {
				animated = false,
				x = state.current_atlas_x,
				y = state.current_atlas_y,
				w = width,
				h = height,
			}

			state.current_atlas_x = state.current_atlas_x + width
			state.max_row_height = math.max(state.max_row_height, height)
		end

		-- Increment progress and print status
		state.clen = state.clen + 1
		print(string.format("Fetched %s [%d/%d]", name, state.clen, state.target_len))

		-- Export atlas + metadata when finished
		if state.clen >= state.target_len then
			export_data()
		end
	end

	-- -----------------------------------------------------------------------
	-- Process the 7TV emote set manifest
	-- -----------------------------------------------------------------------
	local function fetch_emoteset(data)
	-- Decode JSON payload from 7TV API
		data = json.decode(data)
		state.target_len = #data.emotes
		print(string.format("Fetching %d emotes", state.target_len))

		-- Iterate through emotes
		for _, emote in ipairs(data.emotes) do
			local file = emote.data.host.files and emote.data.host.files[1]
			local width, height = 32, 32

			if not file then
			-- Skip if missing file entry
				print("Skipping emote with no file: " .. (emote.name or "unknown"))
			else
				width = file.width or 32
				height = file.height or 32
			end

			-- Choose AVIF for animated, PNG for static
			local url
			if emote.data.animated then
				url = string.format(
					"https://cdn.7tv.app/emote/%s/1x.avif?n=%s&a=1&w=%d&h=%d",
					emote.id, emote.name, width, height
				)
			else
				url = string.format(
					"https://cdn.7tv.app/emote/%s/1x.png?n=%s&w=%d&h=%d",
					emote.id, emote.name, width, height
				)
			end

			-- Dispatch HTTP request via round-robin client
			state.http_clients[state.http_index]:get(url)
			state.http_index = (state.http_index % #state.http_clients) + 1
		end
	end

	-- -----------------------------------------------------------------------
	-- Initialize 4 parallel HTTP clients and the 7TV manifest client
	-- -----------------------------------------------------------------------
	state.http_clients = {
		waywall.http_client_create(process_emote),
		waywall.http_client_create(process_emote),
		waywall.http_client_create(process_emote),
		waywall.http_client_create(process_emote),
	}
	state.http_emoteset = waywall.http_client_create(fetch_emoteset)
	state.emote_atlas = waywall.atlas(2048)

	-- Start downloading the emote set manifest
	state.http_emoteset:get("https://api.7tv.app/v3/emote-sets/" .. id)

	-- Store this instance by its set ID
	M.instances[id] = state
end

-- ---------------------------------------------------------------------------
-- Return module
-- ---------------------------------------------------------------------------
return M
