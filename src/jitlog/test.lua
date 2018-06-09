local ffi = require("ffi")
local hasjit = pcall(require, "jit.opt")
local format = string.format
local reader_def = require("jitlog.reader_def")
GC64 = reader_def.GC64
local msgdef = require"jitlog.messages"
local apigen = require"jitlog.generator"
local readerlib = require("jitlog.reader")
assert(readerlib.makereader())
local jitlog = require("jitlog")

local parser = apigen.create_parser()
parser:parse_msglist(msgdef)
local msginfo_vm = parser:complete()

local function buildmsginfo(msgdefs)
  local parser = apigen.create_parser()
  parser:parse_msglist(msgdefs)
  return parser:complete()
end

local tests = {}

function tests.parser_bitfields()
  local msginfo = buildmsginfo({
  {
    name = "header",
    "majorver : 15",
    "minorver : u8",
    "gc64 : bool",
  }
})

  local header = msginfo.msglist[1]
  assert(header.size == 4)
  assert(#header.fields == 4)
  
  assert(header.fields[1].offset == 0)
  assert(not header.fields[1].bitstorage)
  assert(not header.fields[1].bitofs)
  assert(not header.fields[1].bitsize)
  
  assert(header.fields[2].bitstorage == "header")
  assert(header.fields[2].bitofs == 8)
  assert(header.fields[2].bitsize == 15)
  
  assert(header.fields[3].bitstorage == "header")
  assert(header.fields[3].bitofs == 23)
  assert(header.fields[3].bitsize == 8)
  
  assert(header.fields[4].bitstorage == "header")
  assert(header.fields[4].bitofs == 31)
  assert(header.fields[4].bitsize == 1)
end

function tests.parser_msgheader_overflow()
  local msginfo = buildmsginfo({
    {
      name = "header",
      "majorver : 17",
      "minorver : u8", 
    }
  })

  local header = msginfo.msglist[1]
  assert(header.size == 5)
  assert(#header.fields == 3)
  
  assert(header.fields[1].offset == 0)
  assert(not header.fields[1].bitstorage)
  assert(not header.fields[1].bitofs)
  assert(not header.fields[1].bitsize)
  
  assert(header.fields[2].bitstorage == "header")
  assert(header.fields[2].bitofs == 8)
  assert(header.fields[2].bitsize == 17)
  
  assert(header.fields[3].offset == 4)
  assert(not header.fields[3].bitstorage)
  assert(not header.fields[3].bitofs)
  assert(not header.fields[3].bitsize)
end

function tests.parser_basicheader()
  local msginfo = buildmsginfo({
    {
      name = "header",
      "version : u32",
    }
  })

  assert(#msginfo.msglist == 1)
  local header = msginfo.msglist[1]
  assert(header.name == "header")
  assert(header.size == 8)
  assert(#header.fields == 2)
  
  assert(header.fields[1].offset == 0)
  assert(header.fields[1].name == "header")
  
  assert(header.fields[2].name == "version")
  assert(header.fields[2].offset == 4)
  assert(not header.fields[2].bitstorage)
end

function tests.msgsizes()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = "MSG_"..def.name
    if not def.size or def.size < 4 then
      error(format("Bad message size for message %s ", msgname, def.size or "nil"))
    end
  end
end

function tests.field_offsets()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = "MSG_"..def.name
    local msgsize = def.size

    for _, f in ipairs(def.fields) do
      local name = f.name
      if not f.bitstorage and not f.vlen then
        if not f.offset then
          error(format("Field '%s' in message %s is missing an offset", name, msgname))
        end
        if f.offset >= msgsize then
          error(format("Field '%s' in message %s has a offset %d larger than message size of %d", name, msgname, f.offset, msgsize))
        end
        local offset = ffi.offsetof(msgname, f.name)
        if not offset  then
          error(format("Field '%s' is missing in message %s", name, msgname))
        end
        if offset ~= f.offset then
          error(format("Bad field offset for '%s' in message %s expected %d was %d", name, msgname, offset, f.offset))
        end
      else
        if f.offset then
          error(format("Special field '%s' in message %s has a offset %d when it should have none", name, msgname, f.offset))
        end
      end
    end
  end
end

local function checkheader(header)
  assert(header)
  assert(header.os == jit.os)
  assert(header.version > 0)
end



local testmixins = {
  readerlib.mixins.msgstats,
}

local function parselog(log, verbose)
  local result = readerlib.makereader(testmixins)
  if verbose then
    result.verbose = true
  end
  assert(result:parse_buffer(log, #log))
  checkheader(result.header)
  return result
end

function tests.header()
  jitlog.start()
  local result = parselog(jitlog.savetostring())
  checkheader(result.header)
end

function tests.savetofile()
  jitlog.start()
  jitlog.save("jitlog.bin")
  local result = readerlib.parsefile("jitlog.bin")
  checkheader(result.header)
end

function tests.reset()
  jitlog.start()
  local headersize = jitlog.getsize()
  jitlog.addmarker("marker")
  -- Should have grown by at least 10 = 6 chars + 4 byte msg header
  assert(jitlog.getsize()-headersize >= 10)
  local log1 = jitlog.savetostring()
  -- Clear the log and force a new header to be written
  jitlog.reset()
  assert(jitlog.getsize() == headersize)
  local log2 = jitlog.savetostring()
  assert(#log1 > #log2)

  local result1 = parselog(log1)
  local result2 = parselog(log2)
  assert(result1.starttime < result2.starttime)
end

function tests.stringmarker()
  jitlog.start()
  jitlog.addmarker("marker1")
  jitlog.addmarker("marker2", 0xbeef)
  local result = parselog(jitlog.savetostring())
  assert(result.msgcounts.stringmarker == 2)
  assert(#result.markers == 2)
  assert(result.markers[1].label == "marker1")
  assert(result.markers[2].label == "marker2")
  assert(result.markers[1].eventid < result.markers[2].eventid)
  assert(result.markers[1].time < result.markers[2].time)
  assert(result.markers[1].flags == 0)
  assert(result.markers[2].flags == 0xbeef)
end

if hasjit then

function tests.tracexits()
  jitlog.start()
  local a = 0 
  for i = 1, 200 do
    if i <= 100 then
      a = a + 1
    end
  end
  assert(a == 100)
  local result = parselog(jitlog.savetostring())
  assert(result.exits > 4)
  assert(result.msgcounts.traceexit_small == result.exits)
end

function tests.userflush()
  jitlog.start()
  jit.flush()
  local result = parselog(jitlog.savetostring())
  assert(#result.flushes == 1)
  assert(result.msgcounts.alltraceflush == 1)
  assert(result.flushes[1].reason == "user_requested")
  assert(result.flushes[1].time > 0)
end

function tests.trace()
  jitlog.start()
  local a = 0 
  for i = 1, 300 do
    if i >= 100 then
      if i <= 200 then
        a = a + 1
      else
        a = a + 2
      end
    end
  end
  assert(a == 301)
  
  local result = parselog(jitlog.savetostring())
  assert(result.exits > 0)
  assert(#result.aborts == 0)
  local traces = result.traces
  assert(#traces == 3)
  assert(traces[1].eventid < traces[2].eventid)
  assert(traces[1].startpt == traces[1].stoppt)
  assert(traces[2].startpt == traces[2].stoppt)
  assert(traces[3].startpt == traces[3].stoppt)
  assert(traces[1].startpt == traces[2].startpt and traces[2].startpt == traces[3].startpt)
  assert(traces[1].startpt.chunk:find("test.lua"))
  assert(traces[1].stopfunc == traces[2].stopfunc)
  assert(traces[1].stopfunc.proto == traces[1].stoppt)
  assert(traces[1].stopfunc.upvalues.length > 0)
  -- Root traces should have no parent
  assert(traces[1].parentid == 0)
  assert(traces[2].parentid == traces[1].id)
  assert(traces[3].parentid == traces[2].id)
  assert(traces[1].id ~= traces[2].id and traces[2].id ~= traces[3].id)
end

local function nojit_loop(f, n)
  local ret
  n = n or 200
  for i=1, n do
    ret = f()
  end
  return ret
end

jit.off(nojit_loop)

function tests.protobl()
  jitlog.start()
  local ret1 = function() return 1 end
  jit.off(ret1)
  local func = function() return ret1() end
  nojit_loop(func, 100000)
  -- Check we also handle loop blacklisting
  local function loopbl()
    local ret
    for i=1, 50000 do
      ret = ret1()
    end
    return ret
  end
  loopbl()

  local result = parselog(jitlog.savetostring())
  assert(#result.aborts >= 2)
  local blacklist = result.proto_blacklist 
  assert(#blacklist == 2)
  assert(blacklist[1].eventid < blacklist[2].eventid)
  assert(blacklist[1].time < blacklist[2].time)
  -- Function header blacklisted
  assert(blacklist[1].bcindex == 0)
  -- Loop header blacklisted
  assert(blacklist[2].bcindex > 0)
  assert(blacklist[1].proto ~= blacklist[2].proto)
  assert(blacklist[1].proto.chunk:find("test.lua"))
  assert(blacklist[2].proto.chunk:find("test.lua"))
end

end

function tests.gcstate()
  jitlog.start()
  collectgarbage("collect")
  local t = {}
  for i=1, 10000 do
    t[i] = {1, 2, true, false}
  end
  assert(#t == 10000)
  local result = parselog(jitlog.savetostring())
  assert(result.gccount > 0)
  assert(result.gcstatecount > 4)
  assert(result.gcstatecount == result.msgcounts.gcstate)
  assert(result.peakmem > 0)
  assert(result.peakstrnum > 0)
  if hasjit then
    assert(result.exits > 0)
    assert(result.gcexits > 0)
    assert(result.gcexits <= result.exits)
  end
end

function tests.proto()
  jitlog.start()
  loadstring("return 1")
  loadstring("\nreturn 1, 2")
  local result = parselog(jitlog.savetostring())
  checkheader(result.header)
  assert(#result.protos == 2)
  
  local pt1, pt2 = result.protos[1], result.protos[2]  
  assert(pt1.firstline == 0)
  assert(pt2.firstline == 0)
  assert(pt1.numline == 1)
  assert(pt2.numline == 2)
  assert(pt1.chunk == "return 1")
  assert(pt2.chunk == "\nreturn 1, 2")
  assert(pt1.bclen == 3)
  assert(pt2.bclen == 4)
  -- Top level chunks are vararg
  assert(pt1:get_bcop(0) == "FUNCV")
  assert(pt2:get_bcop(0) == "FUNCV")
  assert(pt1:get_bcop(pt1.bclen-1) == "RET1")
  assert(pt2:get_bcop(pt2.bclen-1) == "RET")
  for i = 1, pt1.bclen-1 do
    assert(pt1:get_linenumber(i) == 1)
  end
  for i = 1, pt2.bclen-1 do
    assert(pt2:get_linenumber(i) == 2)
  end
end

function tests.protoloaded()
  jitlog.start()
  loadstring("return 1")
  loadstring("\nreturn 2")
  local result = parselog(jitlog.savetostring())
  assert(#result.protos == 2)
  assert(result.protos[1].created)
  assert(result.protos[2].created > result.protos[1].created)
  assert(result.protos[2].createdid > result.protos[1].createdid)
end



local failed = false

for name, test in pairs(tests) do
  io.stdout:write("Running: "..name.."\n")
  local success, err = pcall(test)
  if not success then
    failed = true
    io.stderr:write("  FAILED ".. err.."\n")
  end
  pcall(jitlog.shutdown)
end

if failed then
  -- Signal that we failed to travis
  os.exit(1)
end
