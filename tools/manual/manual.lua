-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 The OpenDigitalRadio Authors
--
-- Pandoc filter for the PDF user manual (tools/build_manual.sh). The chapters in docs/user/ are written for GitHub,
-- so they use HTML <img> tags for sized screenshots and relative links between files. This filter makes them work in
-- one PDF:
--   - <img src=... width=... alt=...> becomes a pandoc image, its path made absolute from the repository root (the
--     typst --root), its width converted from screen pixels to print size;
--   - other raw HTML (<p>, </p>, <br>, <sub>) is dropped, keeping the text pandoc parsed around it;
--   - a link to a whole chapter ("#03-first-bootmd" after --file-scope) goes to the chapter's first heading;
--   - a link to a file outside the manual (../dev/..., ../spec/..., ../../tools/...) points at the repository web
--     page when ODR_REPO_URL is set (for example https://github.com/OWNER/REPO/blob/v1.0), else becomes plain text.
-- Environment: ODR_MANUAL_DIR, the chapters' directory relative to the repository root (default docs/user);
-- ODR_REPO_ROOT, the repository root, to leave out images that don't exist (with a warning).

local manual_dir = os.getenv("ODR_MANUAL_DIR") or "docs/user"
local repo_root = os.getenv("ODR_REPO_ROOT")
local repo_url = os.getenv("ODR_REPO_URL")
local px_per_inch = 110	-- a 200 px wide screenshot on GitHub prints about 1.8 in wide

-- "a/b/../c" -> "a/c"
local function normalize(path)
	local parts = {}
	for part in path:gmatch("[^/]+") do
		if part == ".." then
			if #parts > 0 then
				table.remove(parts)
			end
		elseif part ~= "." then
			table.insert(parts, part)
		end
	end
	return table.concat(parts, "/")
end

local function repo_path(rel)
	return normalize(manual_dir .. "/" .. rel)
end

local function attr_of(tag, name)
	return tag:match(name .. '%s*=%s*"([^"]*)"') or tag:match(name .. "%s*=%s*'([^']*)'")
end

local function img_from_html(tag)
	local src = attr_of(tag, "src")
	if src == nil then
		return nil
	end
	local alt = attr_of(tag, "alt") or ""
	local width = attr_of(tag, "width")
	local attr = pandoc.Attr("", {}, {})
	if width ~= nil and width:match("^%d+$") then
		attr.attributes["width"] = string.format("%.2fin", tonumber(width) / px_per_inch)
	end
	local path = src
	if not src:match("^%a+://") then
		path = "/" .. repo_path(src)
		-- A screenshot that isn't rendered yet must not break the build: leave it out and say so
		if repo_root ~= nil then
			local f = io.open(repo_root .. path, "rb")
			if f == nil then
				io.stderr:write("manual: missing image " .. path .. ", left out\n")
				return nil
			end
			f:close()
		end
	end
	return pandoc.Image(alt ~= "" and { pandoc.Str(alt) } or {}, path, "", attr)
end

-- First heading of each chapter, by file prefix (pandoc --file-scope prefixes ids with the file name made an
-- identifier, "03-first-bootmd__", and turns a link to the whole file into "#03-first-bootmd")
local first_heading = {}

local function collect(doc)
	for _, block in ipairs(doc.blocks) do
		if block.t == "Header" and block.identifier ~= "" then
			local file = block.identifier:match("^(.-)__")
			if file ~= nil and first_heading[file] == nil then
				first_heading[file] = block.identifier
			end
		end
	end
end

local function fix_link(link)
	local target = link.target
	if target:match("^%a+:") or target:match("^mailto:") then
		return link
	end
	local file = target:match("^#(.*)$")
	if file ~= nil and first_heading[file] ~= nil then
		link.target = "#" .. first_heading[file]
		return link
	end
	if target:sub(1, 1) == "#" then
		return link
	end
	-- A path outside the manual's chapters
	if repo_url ~= nil and repo_url ~= "" then
		local path, anchor = target:match("^([^#]*)(#?.*)$")
		link.target = repo_url .. "/" .. repo_path(path) .. anchor
		return link
	end
	return link.content
end

local function raw_inline(el)
	if el.format ~= "html" then
		return nil
	end
	if el.text:match("^<img") then
		local img = img_from_html(el.text)
		return img ~= nil and img or {}
	end
	return {}
end

local function raw_block(el)
	if el.format ~= "html" then
		return nil
	end
	local images = {}
	for tag in el.text:gmatch("<img[^>]*>") do
		local img = img_from_html(tag)
		if img ~= nil then
			if #images > 0 then
				table.insert(images, pandoc.Space())
			end
			table.insert(images, img)
		end
	end
	if #images > 0 then
		return pandoc.Para(images)
	end
	return {}
end

-- Markdown images (if any) get the same root-relative paths
local function image(el)
	if not el.src:match("^%a+://") and el.src:sub(1, 1) ~= "/" then
		el.src = "/" .. repo_path(el.src)
	end
	return el
end

return {
	{ Pandoc = function(doc) collect(doc) return nil end },
	{ RawInline = raw_inline, RawBlock = raw_block, Image = image },
	{ Link = fix_link },
}
