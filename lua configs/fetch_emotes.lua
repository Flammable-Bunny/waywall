local waywall = require("waywall")
local json = require("dkjson")

local M = {
	instances = {},
}

local function write_file(filename, data)
	local file = io.open(filename, "wb")
	if not file then
		error("Failed to open file: " .. filename)
	end
	file:write(data)
	file:close()
end

function M.Fetch(id)
	local state = {
		atlas_filename = "/home/bunny/.config/waywall/atlas.raw",
		emoteset_filename = "/home/bunny/.config/waywall/emoteset.json",
		emotes_dir = "/home/bunny/.config/waywall/emotes/",
		target_len = 0,
		clen = 0,
		emote_atlas = nil,
		http_emoteset = nil,
		emote_set = {},
		http_index = 1,
		http_clients = {},
		current_atlas_x = 0,
		current_atlas_y = 0,
		max_row_height = 0,
	}

	os.execute("mkdir -p " .. state.emotes_dir)

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

	local function process_emote(data, url)
		local name = url:match("[?&]n=([^&]+)")
		local width = tonumber(url:match("[?&]w=(%d+)")) or 32
		local height = tonumber(url:match("[?&]h=(%d+)")) or 32
		local is_animated = url:match("%.avif") ~= nil

		if not name then
			return
		end

		if is_animated then
			local filename = state.emotes_dir .. name .. ".avif"
			write_file(filename, data)
			state.emote_set[name] = {
				animated = true,
				path = filename,
				w = width,
				h = height,
			}
		else
			if state.current_atlas_x + width > 2048 then
				state.current_atlas_x = 0
				state.current_atlas_y = state.current_atlas_y + state.max_row_height
				state.max_row_height = 0

				if state.current_atlas_y + height > 2048 then
					state.current_atlas_x = 0
					state.current_atlas_y = 0
					state.max_row_height = 0
				end
			end

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

		state.clen = state.clen + 1
		print(string.format("Fetched %s [%d/%d]", name, state.clen, state.target_len))

		if state.clen >= state.target_len then
			export_data()
		end
	end

	local function fetch_emoteset(data)
		data = json.decode(data)
		state.target_len = #data.emotes
		print(string.format("Fetching %d emotes", state.target_len))

		for _, emote in ipairs(data.emotes) do
			local file = emote.data.host.files[1]
if not file then
    print("Skipping emote with no file: " .. (emote.name or "unknown"))
else
    local width = file.width or 32
    local height = file.height or 32
    -- rest unchanged
end


			if emote.data.animated then
				url = string.format("https://cdn.7tv.app/emote/%s/1x.avif?n=%s&a=1&w=%d&h=%d",
					emote.id, emote.name, width, height)
			else
				url = string.format("https://cdn.7tv.app/emote/%s/1x.png?n=%s&w=%d&h=%d",
					emote.id, emote.name, width, height)
			end

			state.http_clients[state.http_index]:get(url)
			state.http_index = state.http_index % #state.http_clients + 1
		end
	end

	state.http_clients = {
		waywall.http_client_create(process_emote),
		waywall.http_client_create(process_emote),
		waywall.http_client_create(process_emote),
		waywall.http_client_create(process_emote),
	}
	state.http_emoteset = waywall.http_client_create(fetch_emoteset)
	state.emote_atlas = waywall.atlas(2048)
	state.http_emoteset:get("https://api.7tv.app/v3/emote-sets/" .. id)

	M.instances[id] = state
end

return M
