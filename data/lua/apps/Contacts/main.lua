-- ══════════════════════════════════════════════════════════════════
-- Contacts — the unified Address Book. One entry per PERSON, linking their
-- MeshCore (radio) and RNS/LXMF (WiFi) identities. Pure UI over
-- lib/contacts.lua (the JSON-backed store) -- this file owns no state of
-- its own beyond the current screen. List/detail/picker structure mirrors
-- data/lua/apps/RNS/main.lua (contacts list + gridnav_body pattern);
-- confirm/import popups mirror Messenger's show_clear_confirm and its
-- import-contact box (data/lua/apps/Messenger/main.lua).
-- ══════════════════════════════════════════════════════════════════

local lvgl = require("lvgl")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")
local utils = require("lib/utils")
local gridnav_body = require("lib/gridnav_body")
local contacts = require("lib/contacts")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HEADER_H = 24

local COL_META   = "#9aa0a6"
local COL_ON     = "#7fe57f"
local COL_OFF    = "#555555"

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

-- ── Header (persistent across screens) ───────────────────────────────────
local header = root:Object {
    w = W, h = HEADER_H, y = 0,
    border_width = 0, pad_left = 4, pad_right = 4, bg_opa = 0,
}
header:clear_flag(lvgl.FLAG.SCROLLABLE)
local header_title = header:Label { text = "Contacts", align = lvgl.ALIGN.LEFT_MID }

local function set_header(title) header_title.text = title or "Contacts" end

-- ── View teardown (mirrors RNS/Messenger's clear_view) ──────────────────
local current_view = nil
local function clear_view()
    nav.reset()
    if current_view then apps.delete_view(current_view); current_view = nil end
end

local show_list, show_detail, show_new_popup, show_import_picker, show_delete_confirm

local function short_hex(h)
    if not h or h == "" then return nil end
    return h:sub(1, 12) .. "..."
end

-- Alphabetical (case-insensitive) copy of contacts:list() -- the store
-- itself stays insertion-ordered; sorting is a display concern only.
local function sorted_entries()
    local list = contacts:list()
    local out = {}
    for i, e in ipairs(list) do out[i] = e end
    table.sort(out, function(a, b) return (a.name or ""):lower() < (b.name or ""):lower() end)
    return out
end

-- ── LIST SCREEN (default) ────────────────────────────────────────────────
show_list = function()
    clear_view()
    set_header("Contacts")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER + nav.SCROLL_FIRST, true)
    current_view = body

    local home_btn = body:Button { w = 56, h = 24 }
    home_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    home_btn:onevent(lvgl.EVENT.RELEASED, function() apps.go_home() end)

    local new_btn = body:Button { w = 52, h = 24 }
    new_btn:Label { text = "New", align = lvgl.ALIGN.CENTER }
    new_btn:onevent(lvgl.EVENT.RELEASED, function() show_new_popup() end)

    local import_btn = body:Button { w = 70, h = 24 }
    import_btn:Label { text = "Import", align = lvgl.ALIGN.CENTER }
    import_btn:onevent(lvgl.EVENT.RELEASED, function() show_import_picker(nil, nil) end)

    local LIST_H = H - HEADER_H - 36
    local list = body:Object {
        w = lvgl.PCT(100), h = LIST_H,
        border_width = 0, pad_all = 0, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.list(list)
    local bind_click = nav.scroll_aware(list)

    local entries = sorted_entries()
    if #entries == 0 then
        list:Label {
            text = "No contacts yet. New or Import to add one.",
            w = lvgl.PCT(100), h = 40, text_color = COL_META,
        }
    end
    for _, e in ipairs(entries) do
        local row = list:Button { w = lvgl.PCT(100), h = 26 }
        row:Label { text = e.name, align = lvgl.ALIGN.LEFT_MID }
        local m_lbl = row:Label { text = "M", align = lvgl.ALIGN.RIGHT_MID, x = -22 }
        m_lbl:set { text_color = e.mesh_pubkey and COL_ON or COL_OFF }
        local r_lbl = row:Label { text = "R", align = lvgl.ALIGN.RIGHT_MID, x = -6 }
        r_lbl:set { text_color = e.rns_lxmf and COL_ON or COL_OFF }
        bind_click(row, function() show_detail(e.id) end)
    end
end

-- ── NEW CONTACT POPUP ────────────────────────────────────────────────────
show_new_popup = function()
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 160, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = W - 30, h = 130, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    local function close() nav.pop(); overlay:delete() end

    box:Label { text = "New contact", w = lvgl.PCT(100), h = 18 }
    local name_ta = box:Textarea {
        one_line = true, placeholder_text = "Name", w = lvgl.PCT(100), h = 32,
    }
    local status = box:Label { text = "", w = lvgl.PCT(100), h = 16, text_color = COL_META }

    local row = box:Object {
        w = lvgl.PCT(100), h = 30, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    row:clear_flag(lvgl.FLAG.SCROLLABLE)
    local save_btn = row:Button { w = lvgl.PCT(48), h = 28 }
    save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
    save_btn:onevent(lvgl.EVENT.RELEASED, function()
        local entry, err = contacts:add({ name = name_ta.text })
        if not entry then
            status.text = err or "Failed"
            return
        end
        close()
        show_list()
    end)
    local cancel_btn = row:Button { w = lvgl.PCT(48), h = 28 }
    cancel_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    cancel_btn:onevent(lvgl.EVENT.RELEASED, close)
end

-- ── IMPORT / LINK PICKER ─────────────────────────────────────────────────
-- target_id == nil: "Import" from the list screen -- picking a candidate
--   CREATES a new entry prefilled with that identity + name.
-- target_id set: "Link from Mesh.../Link from RNS..." from a detail screen
--   -- picking a candidate LINKS that identity INTO the existing entry.
-- origin_filter: nil shows both mesh + rns candidates (the list screen's
--   Import); "mesh"/"rns" restricts to one (a detail screen's per-field
--   Link button), so linking a specific slot can't offer the wrong kind.
show_import_picker = function(target_id, origin_filter)
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0,
        bg_color = "#000000", bg_opa = 180, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = W - 16, h = H - 30, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 6,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    local function close() nav.pop(); overlay:delete() end

    local top = box:Object {
        w = lvgl.PCT(100), h = 26, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    top:clear_flag(lvgl.FLAG.SCROLLABLE)
    top:Label { text = target_id and "Link identity" or "Import contact", w = lvgl.PCT(70) }
    local close_btn = top:Button { w = lvgl.PCT(28), h = 22 }
    close_btn:Label { text = "Close", align = lvgl.ALIGN.CENTER }
    close_btn:onevent(lvgl.EVENT.RELEASED, close)

    local list = box:Object {
        w = lvgl.PCT(100), h = lvgl.PCT(100) - 30,
        border_width = 0, pad_all = 0, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.list(list)
    local bind_click = nav.scroll_aware(list)

    -- Selecting a candidate either creates a new entry (target_id == nil)
    -- or links into the entry being edited (target_id set).
    local function choose(cand)
        if target_id then
            local ok, err
            if cand.origin == "mesh" then
                ok, err = contacts:link_mesh(target_id, cand.mesh_pubkey)
            else
                ok, err = contacts:link_lxmf(target_id, cand.rns_lxmf)
            end
            close()
            if not ok then utils.createNotification(root, err or "Link failed", 2200) end
            show_detail(target_id)
        else
            local fields = {
                name = (cand.name ~= "" and cand.name)
                    or ("(" .. (cand.mesh_pubkey or cand.rns_lxmf):sub(1, 10) .. ")"),
            }
            if cand.mesh_pubkey then fields.mesh_pubkey = cand.mesh_pubkey end
            if cand.rns_lxmf then fields.rns_lxmf = cand.rns_lxmf end
            local entry, err = contacts:add(fields)
            close()
            if entry then
                show_detail(entry.id)
            else
                utils.createNotification(root, err or "Import failed", 2200)
                show_list()
            end
        end
    end

    local function add_row(prefix, cand)
        local row = list:Button { w = lvgl.PCT(100), h = 26 }
        local label = prefix .. ((cand.name ~= "" and cand.name)
            or ("(" .. (cand.mesh_pubkey or cand.rns_lxmf):sub(1, 10) .. ")"))
        row:Label { text = label, align = lvgl.ALIGN.LEFT_MID }
        bind_click(row, function() choose(cand) end)
    end

    local mesh_cands = (origin_filter == "rns") and {} or contacts:candidates_from_mesh()
    local rns_cands = (origin_filter == "mesh") and {} or contacts:candidates_from_rns()
    for _, c in ipairs(mesh_cands) do add_row("Mesh: ", c) end
    for _, c in ipairs(rns_cands) do add_row("RNS: ", c) end
    if #mesh_cands == 0 and #rns_cands == 0 then
        list:Label {
            text = "No new identities found.", w = lvgl.PCT(100), h = 30, text_color = COL_META,
        }
    end
end

-- ── DELETE CONFIRM ───────────────────────────────────────────────────────
show_delete_confirm = function(id)
    local entry = contacts:get(id)
    if not entry then return end
    local overlay = root:Object {
        w = W, h = H, x = 0, y = 0, bg_opa = 200, border_width = 0, pad_all = 0,
    }
    overlay:clear_flag(lvgl.FLAG.SCROLLABLE)
    overlay:add_flag(lvgl.FLAG.CLICKABLE)
    local box = overlay:Object {
        w = 220, h = 100, align = lvgl.ALIGN.CENTER,
        border_width = 1, pad_all = 10,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    box:clear_flag(lvgl.FLAG.SCROLLABLE)
    nav.push(box)

    box:Label { text = "Delete " .. (entry.name or "") .. "?", w = lvgl.PCT(100), h = 24 }
    local yes_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    yes_btn:Label { text = "Delete", align = lvgl.ALIGN.CENTER }
    yes_btn:onevent(lvgl.EVENT.RELEASED, function()
        contacts:remove(id)
        nav.pop(); overlay:delete()
        show_list()
    end)
    local no_btn = box:Button { w = lvgl.PCT(48), h = 32 }
    no_btn:Label { text = "Cancel", align = lvgl.ALIGN.CENTER }
    no_btn:onevent(lvgl.EVENT.RELEASED, function() nav.pop(); overlay:delete() end)
end

-- ── DETAIL SCREEN ─────────────────────────────────────────────────────────
show_detail = function(id)
    local entry = contacts:get(id)
    if not entry then show_list(); return end
    clear_view()
    set_header(entry.name)

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER + nav.SCROLL_FIRST, true)
    current_view = body

    local back = body:Button { w = 60, h = 24 }
    back:Label { text = "< Back", align = lvgl.ALIGN.CENTER }
    back:onevent(lvgl.EVENT.RELEASED, function() show_list() end)

    local pane = body:Object {
        w = lvgl.PCT(100), h = H - HEADER_H - 30,
        border_width = 0, pad_all = 4, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }

    -- Name (editable) ------------------------------------------------------
    pane:Label { text = "Name", w = lvgl.PCT(100), h = 14, text_color = COL_META }
    local name_row = pane:Object {
        w = lvgl.PCT(100), h = 30, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    name_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    local name_ta = name_row:Textarea { one_line = true, text = entry.name, w = lvgl.PCT(70), h = 28 }
    local name_save = name_row:Button { w = lvgl.PCT(28), h = 28 }
    name_save:Label { text = "Save", align = lvgl.ALIGN.CENTER }
    name_save:onevent(lvgl.EVENT.RELEASED, function()
        local ok, err = contacts:update(id, { name = name_ta.text })
        utils.createNotification(root, ok and "Saved" or (err or "Save failed"), 1500)
        if ok then set_header(name_ta.text) end
    end)

    -- Mesh identity ----------------------------------------------------------
    pane:Label { text = "Mesh (radio) identity", w = lvgl.PCT(100), h = 14, text_color = COL_META }
    local mesh_row = pane:Object {
        w = lvgl.PCT(100), h = 26, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    mesh_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    mesh_row:Label { text = short_hex(entry.mesh_pubkey) or "Not linked", w = lvgl.PCT(55), align = lvgl.ALIGN.LEFT_MID }
    if entry.mesh_pubkey then
        local unlink_btn = mesh_row:Button { w = lvgl.PCT(43), h = 24 }
        unlink_btn:Label { text = "Unlink", align = lvgl.ALIGN.CENTER }
        unlink_btn:onevent(lvgl.EVENT.RELEASED, function() contacts:unlink_mesh(id); show_detail(id) end)
    else
        local link_btn = mesh_row:Button { w = lvgl.PCT(43), h = 24 }
        link_btn:Label { text = "Link Mesh...", align = lvgl.ALIGN.CENTER }
        link_btn:onevent(lvgl.EVENT.RELEASED, function() show_import_picker(id, "mesh") end)
    end

    -- RNS (LXMF) identity ------------------------------------------------
    pane:Label { text = "RNS (LXMF) identity", w = lvgl.PCT(100), h = 14, text_color = COL_META }
    local rns_row = pane:Object {
        w = lvgl.PCT(100), h = 26, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    rns_row:clear_flag(lvgl.FLAG.SCROLLABLE)
    rns_row:Label { text = short_hex(entry.rns_lxmf) or "Not linked", w = lvgl.PCT(55), align = lvgl.ALIGN.LEFT_MID }
    if entry.rns_lxmf then
        local unlink_btn = rns_row:Button { w = lvgl.PCT(43), h = 24 }
        unlink_btn:Label { text = "Unlink", align = lvgl.ALIGN.CENTER }
        unlink_btn:onevent(lvgl.EVENT.RELEASED, function() contacts:unlink_lxmf(id); show_detail(id) end)
    else
        local link_btn = rns_row:Button { w = lvgl.PCT(43), h = 24 }
        link_btn:Label { text = "Link RNS...", align = lvgl.ALIGN.CENTER }
        link_btn:onevent(lvgl.EVENT.RELEASED, function() show_import_picker(id, "rns") end)
    end

    -- Message (RNS): apps.launch(name) cleanly supports launching another
    -- app by name with no arguments (data/lua/lib/apps.lua:567 launch()),
    -- so this button is safe to add per the task spec. GAP (reported, not
    -- fixed here): launch() has NO channel to hand the target app a start
    -- parameter -- loadfile(rec.entrypoint) / _dofile_sd(entry, rec.dir)
    -- only ever pass the app's OWN directory, never caller data -- so this
    -- opens the RNS app on its default Announces screen, not this
    -- contact's specific thread. Deep-linking would need an apps.lua
    -- change, out of scope here (Lua-only, but apps.lua wasn't part of
    -- this task's file list and changing shared nav plumbing needs its
    -- own review).
    if entry.rns_lxmf then
        local msg_btn = pane:Button { w = lvgl.PCT(60), h = 28 }
        msg_btn:Label { text = "Message (RNS)", align = lvgl.ALIGN.CENTER }
        msg_btn:onevent(lvgl.EVENT.RELEASED, function() apps.launch("RNS") end)
    end

    local del_btn = pane:Button { w = lvgl.PCT(60), h = 28 }
    del_btn:Label { text = "Delete entry", align = lvgl.ALIGN.CENTER }
    del_btn:onevent(lvgl.EVENT.RELEASED, function() show_delete_confirm(id) end)
end

-- ── Boot ─────────────────────────────────────────────────────────────────
-- No apps.set_on_close needed: unlike RNS/main.lua this app registers no
-- live native-dispatch callbacks to clear (lib/contacts.lua is a plain
-- on-demand store, not an event sink) -- same as Settings/Names/main.lua.
show_list()

return root
