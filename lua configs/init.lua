 -- STUFF YOU NEED TO ADD TO YOUR CONFIG!! --

local emote_downloader = require("fetch_emotes")
local chat = require("chat")
local chat1 = chat("Flammable_Bunny", 15, 700, 16)


local config = {
    theme = {
		font_path = "/usr/share/fonts/TTF/JetBrainsMono-Medium.ttf",
        font_size = 25,
    },

}

local open_chat_key = "Shift-U",
local emote_key = "Shift-Y",

config.actions {

	[emote_key] = function()
        emote_downloader.Fetch("XXXXXXXXXXXXXXXXX") -- REPLACE THE X's WITH YOUR 7TV EMOTESET URL TAG
    end,

	[open_chat_key] = function()
	    chat1:open()
	end,
}


return config
