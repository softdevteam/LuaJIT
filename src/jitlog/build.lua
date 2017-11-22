local stdout = io.stdout
local args = {...}

GC64 = args[1] == "GC64"

--Work around the limited API when run under minilua
if not require then
  function print(...)
    local t = {...}
    for i, v in ipairs(t) do
      if i > 1 then
        stdout:write("\t")
      end
      if type(v) == "boolean" then
        stdout:write((v and "true") or "false")
      else
        stdout:write(v)
      end
    end
    stdout:write("\n")
  end

  function require(modulename)
    local path = modulename..".lua"

    if string.find(modulename, "%.") then
      local package, name = string.match(modulename, "([^%.]+)%.(.+)")
      
      if not package or not name then
        error("bad lua module name")
      end
      path = package .. "/".. name..".lua"
    end
  
    local fp = assert(io.open(path))
    local s = fp:read("*a")
    assert(fp:close())
    return assert(loadstring(s, "@"..modulename..".lua"))()
  end
end

local msgdef = require"jitlog.messages"
local apigen = require"jitlog.generator"
local parser = apigen.create_parser()
parser:parse_msglist(msgdef)

local data = parser:complete()
apigen.write_c(data)
