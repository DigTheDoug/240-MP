local assdraw = require 'mp.assdraw'

local menu_visible = false
local focus_btn = 1  -- index into buttons (CROP when shown, then STOP)
local update_timer = nil
local idle_timer = nil

local MENU_TIMEOUT = 5

local C_WHITE = "&HFFFFFF&"
local C_BLACK = "&H000000&"
local A_OPAQUE = "&H00&"
local A_TRANS  = "&HFF&"

local function draw_rect(ass, x, y, w, h, fc, fa, bs, bc)
    ass:new_event()
    ass:pos(x, y)
    ass:append(string.format(
        "{\\bord%d\\3c%s\\3a&H00&\\1c%s\\1a%s\\shad0}",
        bs, bc, fc, fa))
    ass:draw_start()
    ass:rect_cw(0, 0, w, h)
    ass:draw_stop()
end

local function draw_text(ass, x, y, anchor, text, fs, fc, fa)
    ass:new_event()
    ass:append(string.format(
        "{\\an%d\\pos(%d,%d)\\fnVCR OSD Mono\\fs%d\\1c%s\\1a%s\\shad0\\bord0}%s",
        anchor, x, y, fs, fc, fa, text))
end

-- Set by MpvController with the resolved shader paths and the crt_filter
-- setting's value at launch — absent entirely on the Pi headless --vo=drm
-- paths, where --glsl-shaders has no effect, so the button hides there.
local crt_shader_regular = mp.get_opt("crt-shader-regular")
local crt_shader_heavy   = mp.get_opt("crt-shader-heavy")
local crt_available = (crt_shader_regular or "") ~= "" and (crt_shader_heavy or "") ~= ""
local crt_order = {"Off", "Regular", "Heavy"}
local crt_state = mp.get_opt("crt-filter-initial")
if crt_state ~= "Regular" and crt_state ~= "Heavy" then crt_state = "Off" end

local function crt_shader_path(state)
    if state == "Regular" then return crt_shader_regular end
    if state == "Heavy"   then return crt_shader_heavy   end
    return nil
end

-- Session-only: see mpv-osc.lua's cycle_crt for the full rationale — cycles
-- Off -> Regular -> Heavy live, does not persist back to the crt_filter setting.
local function cycle_crt()
    local idx = 1
    for i, v in ipairs(crt_order) do
        if v == crt_state then idx = i break end
    end
    crt_state = crt_order[(idx % #crt_order) + 1]
    local path = crt_shader_path(crt_state)
    if path then
        mp.commandv("no-osd", "change-list", "glsl-shaders", "set", path)
    else
        mp.commandv("no-osd", "change-list", "glsl-shaders", "clr", "")
    end
end

-- CROP is omitted when MpvController flags a decode path where --panscan
-- blanks the video (Pi 3 overlay path with 1080p Playback ON).
local buttons = {}
if mp.get_opt("hide-crop") ~= "1" then
    buttons[#buttons + 1] = { label = "CROP", action = function() mp.command("no-osd cycle-values panscan 0 1") end }
end
if crt_available then
    buttons[#buttons + 1] = { label = "CRT", action = cycle_crt }
end
buttons[#buttons + 1] = { label = "STOP", action = function() mp.command("quit") end }

local function draw_menu()
    local ass = assdraw.ass_new()
    local ww, wh = mp.get_osd_size()
    if ww == 0 or wh == 0 then return end

    local fs      = math.floor(wh * 0.0333333)
    local lm      = math.floor(ww * 0.12)
    local rm      = math.floor(ww * 0.88)
    local bar_w   = rm - lm
    local border  = 2
    local btn_h   = math.floor(fs * 1.5)
    local btn_gap = math.floor(bar_w * 0.025)
    local btn_y   = math.floor(wh * 0.8333333)
    local btn_w   = math.floor(bar_w * 0.090625)

    local bx = lm
    for i, btn in ipairs(buttons) do
        local sel    = (focus_btn == i)
        local fill_c = sel and C_WHITE or C_BLACK
        local fill_a = sel and A_OPAQUE or A_TRANS
        local text_c = sel and C_BLACK  or C_WHITE

        draw_rect(ass, bx, btn_y, btn_w, btn_h, fill_c, fill_a, border, C_WHITE)
        draw_text(ass, bx + btn_w / 2, btn_y + btn_h / 2, 5,
                  btn.label, fs, text_c, A_OPAQUE)
        bx = bx + btn_w + btn_gap
    end

    mp.set_osd_ass(ww, wh, ass.text)
end

local function reset_idle_timer()
    if idle_timer then idle_timer:kill() end
    idle_timer = mp.add_timeout(MENU_TIMEOUT, function()
        if menu_visible then
            menu_visible = false
            mp.set_osd_ass(0, 0, "")
            if update_timer then update_timer:stop() end
            mp.remove_key_binding("menu-left")
            mp.remove_key_binding("menu-right")
            mp.remove_key_binding("menu-enter")
            mp.remove_key_binding("menu-esc")
            mp.remove_key_binding("menu-bs")
        end
    end)
end

local function update_nav(action)
    reset_idle_timer()

    if action == "left" then
        focus_btn = focus_btn > 1 and focus_btn - 1 or #buttons
    elseif action == "right" then
        focus_btn = focus_btn < #buttons and focus_btn + 1 or 1
    elseif action == "enter" then
        buttons[focus_btn].action()
        return
    end

    draw_menu()
end

local function toggle_menu()
    if menu_visible then
        menu_visible = false
        mp.set_osd_ass(0, 0, "")
        if update_timer then update_timer:stop() end
        if idle_timer   then idle_timer:kill()   end
        mp.remove_key_binding("menu-left")
        mp.remove_key_binding("menu-right")
        mp.remove_key_binding("menu-enter")
        mp.remove_key_binding("menu-esc")
        mp.remove_key_binding("menu-bs")
    else
        menu_visible = true
        draw_menu()
        update_timer = mp.add_periodic_timer(0.5, draw_menu)
        reset_idle_timer()

        mp.add_forced_key_binding("LEFT",  "menu-left",  function() update_nav("left")  end)
        mp.add_forced_key_binding("RIGHT", "menu-right", function() update_nav("right") end)
        mp.add_forced_key_binding("ENTER", "menu-enter", function() update_nav("enter") end)
        mp.add_forced_key_binding("ESC",   "menu-esc",   toggle_menu)
        mp.add_forced_key_binding("BS",    "menu-bs",    toggle_menu)
    end
end

mp.add_forced_key_binding("UP",   "open_menu_up",   toggle_menu)
mp.add_forced_key_binding("DOWN", "open_menu_down", toggle_menu)

mp.add_key_binding("ESC", "bg-esc", function() mp.command("quit") end)
mp.add_key_binding("BS",  "bg-bs",  function() mp.command("quit") end)
