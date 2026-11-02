-- Manual, single-shot Canon autofocus proof of concept.
-- Install as ML/SCRIPTS/LENS_AF.LUA.
-- The script body only creates the menu; autofocus starts after manual Run.

local lens_af_menu
local status = "IDLE"
local busy = false
local lens_called = false
local last_result = nil

local function notify(message)
    local text = "LENS_AF: " .. tostring(message)

    status = tostring(message)

    pcall(function()
        display.notify_box(text, 5000)
    end)
    pcall(function()
        console.show()
        print(text)
    end)
end

local function status_text()
    return string.format(
        "%s | lens_called=%s | result=%s",
        tostring(status),
        tostring(lens_called),
        tostring(last_result))
end

local function read_state()
    return {
        af = lens.af,
        autofocusing = lens.autofocusing,
        af_mode = lens.af_mode,
        lv_running = lv.running,
        lv_enabled = lv.enabled,
        camera_mode = camera.mode,
        movie_recording = movie.recording,
        focus_pos = lens.focus_pos,
        focus_distance = lens.focus_distance,
    }
end

local function state_text(state)
    return string.format(
        "af=%s autofocusing=%s af_mode=%s lv.running=%s lv.enabled=%s camera.mode=%s movie.recording=%s focus_pos=%s focus_distance=%s",
        tostring(state.af),
        tostring(state.autofocusing),
        tostring(state.af_mode),
        tostring(state.lv_running),
        tostring(state.lv_enabled),
        tostring(state.camera_mode),
        tostring(state.movie_recording),
        tostring(state.focus_pos),
        tostring(state.focus_distance))
end

local function run_lens_af_operation()
    local before
    local after
    local started
    local elapsed
    local result

    if type(lens.autofocus) ~= "function" then
        notify("BLOCKED: lens.autofocus() unavailable; lens_called=false")
        return
    end

    before = read_state()
    notify("STARTED: task running; lens_called=false; lv.running=" .. tostring(before.lv_running) .. "; camera.mode=" .. tostring(before.camera_mode) .. "; movie.recording=" .. tostring(before.movie_recording))

    if before.af ~= true then
        notify("BLOCKED: lens AF is disabled; lens_called=false")
        return
    end

    if before.autofocusing == true then
        notify("BLOCKED: autofocus already in progress; lens_called=false")
        return
    end

    if before.lv_running ~= true then
        notify("BLOCKED: LiveView must be running; lens_called=false")
        return
    end

    if before.camera_mode == MODE.MOVIE then
        notify("BLOCKED: Movie mode; lens_called=false")
        return
    end

    if before.movie_recording == true then
        notify("BLOCKED: recording; lens_called=false")
        return
    end

    notify("STARTED: lens.autofocus() about to be called; lens_called=false")
    started = dryos.ms_clock
    lens_called = true
    result = lens.autofocus()
    elapsed = dryos.ms_clock - started
    last_result = result

    after = read_state()
    if type(result) ~= "boolean" then
        notify(string.format(
            "ERROR: classification=LUA_ERROR lens.autofocus() returned %s; lens_called=true elapsed_ms=%s; after: %s",
            type(result),
            tostring(elapsed),
            state_text(after)))
    elseif result then
        notify(string.format(
            "COMPLETED: classification=IDLE_COMPLETED result=true lens_called=true elapsed_ms=%s; after: %s",
            tostring(elapsed),
            state_text(after)))
    else
        notify(string.format(
            "COMPLETED: classification=COMMAND_RETURNED_FALSE_OR_FOCUSING_STATE_NOT_OBSERVED result=false lens_called=true elapsed_ms=%s; after: %s",
            tostring(elapsed),
            state_text(after)))
    end
end

local function run_lens_af_task()
    local ok
    local error_message

    ok, error_message = pcall(run_lens_af_operation)
    busy = false

    if not ok then
        notify("ERROR: " .. tostring(error_message) .. "; lens_called=" .. tostring(lens_called))
    end
end

local function select_lens_af()
    if busy then
        notify("BLOCKED: busy; lens_called=false")
        return
    end

    busy = true
    lens_called = false
    last_result = nil
    notify("STARTED: callback executed; lens_called=false")

    local ok, error_message = pcall(function()
        task.create(run_lens_af_task)
    end)

    if not ok then
        busy = false
        notify("ERROR: task.create failed: " .. tostring(error_message) .. "; lens_called=false")
    end
end

lens_af_menu = menu.new
{
    parent = "Scripts",
    name = "Lens AF PoC",
    help = "Manual, single-shot Canon autofocus test; no autorun.",
    submenu =
    {
        {
            name = "Run",
            help = "Run one lens.autofocus() operation.",
            select = select_lens_af,
        },
    },
}
