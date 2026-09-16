-- snap.lua - boot the FLY test disk and take a screenshot every SNAP_EVERY
-- seconds for SNAP_TOTAL seconds. Snapshots land in -snapshot_directory.
local every = tonumber(os.getenv("SNAP_EVERY") or "4")
local total = tonumber(os.getenv("SNAP_TOTAL") or "120")
local keys = os.getenv("SNAP_KEYS") or ""   -- "t=key,t=key" ADB presses
local mch = manager.machine
local t0 = nil
local nextshot = 0
local done = false
local presses = {}
for item in keys:gmatch("[^,]+") do
	local t, k = item:match("^(%d+%.?%d*)=(.+)$")
	if t then presses[#presses + 1] = { t = tonumber(t), k = k, sent = false } end
end

local function find_field(port_tag, want)
	local port = mch.ioport.ports[port_tag]
	if not port then return nil end
	for name, field in pairs(port.fields) do
		if name:sub(1, #want) == want then return field end
	end
	return nil
end

-- ADB key by MAME field name ("Esc", "Space", "A"...): hold for 0.3 s
local function adb_key(name, down)
	for i = 0, 7 do
		local f = find_field(":macadb:KEY" .. i, name)
		if f then f:set_value(down) return true end
	end
	return false
end

local releases = {}
local function post(s)
	if s:sub(1, 1) == "@" then
		local name = s:sub(2)
		emu.print_info("SNAP: adb " .. name .. " " .. tostring(adb_key(name, 1)))
		releases[#releases + 1] = { t = mch.time.seconds + 0.3, name = name }
	else
		pcall(function() mch.natkeyboard:post(s) end)
	end
end

emu.register_frame_done(function()
	if done then return end
	local now = mch.time.seconds
	if not t0 then t0 = now end
	local el = now - t0
	if el >= nextshot then
		mch.video:snapshot()
		emu.print_info(string.format("SNAP: t=%.0f", el))
		nextshot = nextshot + every
	end
	for _, p in ipairs(presses) do
		if not p.sent and el >= p.t then
			p.sent = true
			emu.print_info(string.format("SNAP: t=%.0f press %s", el, p.k))
			post(p.k)
		end
	end
	for _, r in ipairs(releases) do
		if r.name and now >= r.t then adb_key(r.name, 0); r.name = nil end
	end
	if el >= total then
		done = true
		mch:exit()
	end
end)
