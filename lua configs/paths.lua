local toggles = require("toggles")

local home = os.getenv("HOME")
local waywall_config = home .. "/.config/waywall/"

local paths = {
    bg_path = toggles.toggle_bg_picture and (home .. "/.config/waywall/images/background.png") or nil,
    pacem_path   = home .. "/mcsr-apps/paceman-tracker-0.7.1.jar",
    nb_path      = home .. "/mcsr-apps/Ninjabrain-Bot-1.5.1.jar",
    overlay_path = home .. "/.config/waywall/images/measuring_overlay.png",
    lingle_path  = home .. "/IdeaProjects/Lingle/build/libs/Lingle-v1.0.0.jar",
}

return paths
