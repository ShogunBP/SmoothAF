-- Read-only autofocus state observer.
-- Install as ML/SCRIPTS/LENS_AF_OBSERVER.LUA.
-- Press the physical half-shutter while the bounded observation is running.

local observer_menu
local observing = false
local status = "IDLE"
local last_state = "No observation started"

local function notify(message)
    local text = "LENS_AF_OBSERVER: " .. tostring(message)
    status = tostring(message)
    last_state = text

    pcall(function()
        display.notify_box(text, 3000)
    end)
    pcall(function()
        console.show()
        print(text)
    end)
end

local function read_state()
    return {
        autofocus = lens.autofocusing,
        af = lens.af,
        af_mode = lens.af_mode,
        lv_running = lv.running,
        lv_enabled = lv.enabled,
        camera_mode = camera.mode,
        movie_recording = movie.recording,
        focus_distance = lens.focus_distance,
    }
end

local function state_text(state)
    return string.format(
        "autofocusing=%s af=%s af_mode=%s lv.running=%s lv.enabled=%s camera.mode=%s movie.recording=%s focus_distance=%s",
        tostring(state.autofocus),
        tostring(state.af),
        tostring(state.af_mode),
        tostring(state.lv_running),
        tostring(state.lv_enabled),
        tostring(state.camera_mode),
        tostring(state.movie_recording),
        tostring(state.focus_distance))
end

local function observe_task()
    local ok, error_message = pcall(function()
        local before = read_state()
        notify("STARTED; press physical half-shutter now; before: " .. state_text(before))

        for sample = 1, 600 do
            msleep(10)
            local state = read_state()
            if state.autofocus ~= before.autofocus then
                notify("TRANSITION sample=" .. tostring(sample) .. "; " .. state_text(state))
                before = state
            end
        end

        notify("COMPLETED; no more samples; final: " .. state_text(before))
    end)

    observing = false
    if not ok then
        notify("ERROR: " .. tostring(error_message))
    end
end

local function start_observation()
    if observing then
        notify("BLOCKED: observation already running")
        return
    end

    observing = true
    notify("STARTED: scheduling read-only observation")
    local ok, error_message = pcall(function()
        task.create(observe_task)
    end)
    if not ok then
        observing = false
        notify("ERROR: task.create failed: " .. tostring(error_message))
    end
end

observer_menu = menu.new
{
    parent = "Scripts",
    name = "Lens AF Observer",
    help = "Read-only six-second physical half-shutter state observation.",
    submenu =
    {
        {
            name = "Start observation",
            help = "Start one bounded read-only sample window.",
            select = start_observation,
        },
    },
}
