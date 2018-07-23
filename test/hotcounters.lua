local jit = require"jit"
local jit_util = require"jit.util"
local jitlog = require"jitlog"
local readerlib = require("jitlog.reader")
local get_hotcount = jit_util.funchcount
local format = string.format

if not pcall(require, "jit.opt") then
  return
end

local fhot = 56*2
local lhot = 56
local func_penalty = 36*2
local loop_penalty = 36
local maxattemps_func = 4
local maxattemps_loop = 8

local function getmaxcount(attempts, penalty)
  local count = penalty + 16
  for i = 1, attempts-2 do
    count = (count*2) + 16
    if count > 0xffff then
      error("Attempt count of "..count.." for attempt "..i.." is larger than the max value of uint16_t")
    end
  end
  return count
end

local countmax_func = getmaxcount(maxattemps_func, func_penalty)
local countmax_loop = getmaxcount(maxattemps_loop, loop_penalty)
print("penaltymaxfunc="..countmax_func, "penaltymaxloop="..countmax_loop)

jit.opt.start("penaltymaxfunc="..countmax_func, "penaltymaxloop="..countmax_loop)

local function calln(f, n)
  for i=1, n do
    f()
  end
end
jit.off(calln)

local countid_calls = -1
local countid_loops = -2 -- total number of times a loop
local countid_func = 0
local countid_loop1 = 1 -- The value of first LOOPHC bytecode in the function
local countid_loop2 = 2

local function parselog(log, verbose)
  local result = readerlib.makereader()
  if verbose then
    result.verbose = true
  end
  assert(result:parse_buffer(log, #log))
  return result
end

local tests = {}

function tests.func_hotcounters()
  local function f1() return 1 end
  
  assert(get_hotcount(f1, countid_func) == fhot)
  assert(get_hotcount(f1, countid_calls) == 0)
  assert(get_hotcount(f1, countid_loops) == 0)
  
  f1()
  assert(get_hotcount(f1, countid_func) == fhot-1)
  assert(get_hotcount(f1, countid_calls) == 1) -- call count should of incremented
  assert(get_hotcount(f1, countid_loops) == 0)
  
  f1(); f1(); f1(); f1();
  assert(get_hotcount(f1, countid_func) == fhot-5)
  assert(get_hotcount(f1, countid_calls) == 5)
  assert(get_hotcount(f1, countid_loops) == 0)
  
  local function f2() end
  calln(f2, fhot)
  assert(get_hotcount(f2, countid_calls) == fhot, get_hotcount(f2, countid_calls))
  assert(get_hotcount(f2, countid_func) == 0, get_hotcount(f2, countid_func) )
  
  jitlog.start()
  -- Now trigger the trace
  f2()
  assert(get_hotcount(f2, countid_calls) == fhot+1, get_hotcount(f2, countid_calls))
  assert(get_hotcount(f2, countid_func) == fhot, get_hotcount(f2, countid_func))
  
  -- Functinon JIT'ed now
  f2()
  -- Call counter still goes up for JFUNC
  assert(get_hotcount(f2, countid_calls) == fhot+2, get_hotcount(f2, countid_calls))
  -- But the function hot counter should stop changing now its JIT'ed
  assert(get_hotcount(f2, countid_func) == fhot, get_hotcount(f2, countid_func))
  
  local result = parselog(jitlog.savetostring())
  assert(#result.traces == 1)
  assert(result.traces[1].callcount == fhot)
  assert(result.traces[1].startpc == 0)
end

function tests.loop_hotcounters()
  local function f1(n) 
    a = 0 
    for i=1,n do a = a + 1 end
    return a
  end 
  assert(get_hotcount(f1, countid_func) == fhot)
  assert(get_hotcount(f1, countid_loop1) == lhot)
  assert(get_hotcount(f1, countid_calls) == 0)
  assert(get_hotcount(f1, countid_loops) == 0)
  
  f1(2)
  assert(get_hotcount(f1, countid_func) == fhot-1)
  assert(get_hotcount(f1, countid_loop1) == lhot-2)
  assert(get_hotcount(f1, countid_calls) == 1)
  assert(get_hotcount(f1, countid_loops) == 2)
  
  -- Run the loop with an out of range count. no loop counts should change
  f1(-1)
  assert(get_hotcount(f1, countid_loop1) == lhot-2)
  assert(get_hotcount(f1, countid_loops) == 2)
  assert(get_hotcount(f1, countid_func) == fhot-2)
  --jitlog.reset()
  jitlog.start()
  
  f1(lhot-2)
  assert(get_hotcount(f1, countid_loop1) == 0)
  
  -- If the loop trip count is too low the loop trace will abort because the trace 
  -- would have left the loop while still recording it
  f1(3)
  -- Loop hotcounter is reset to the starting hot counter value when it starts a trace
  assert(get_hotcount(f1, countid_loop1) == lhot, get_hotcount(f1, 1))
  
  f1(1)
  assert(get_hotcount(f1, countid_loop1) == lhot)
  assert(get_hotcount(f1, countid_loops) == lhot+1, get_hotcount(f1, countid_loops))
  
  local result = parselog(jitlog.savetostring())
  assert(#result.traces == 1)
  assert(result.traces[1].callcount == lhot, result.traces[1].callcount)
  assert(result.traces[1].startpc ~= 0)
end

function tests.multiloop_hotcounters()
  local function f1(n, m) 
    local a = 0 
    for i=1,n do a = a + 1 end
    local b = 0 
    for i=1,m do b = b + 1 end
    return a, b
  end 
  assert(get_hotcount(f1, countid_loop1) == lhot)
  assert(get_hotcount(f1, countid_loop2) == lhot)
  
  f1(1, -1)
  assert(get_hotcount(f1, countid_loop1) == lhot-1)
  assert(get_hotcount(f1, countid_loop2) == lhot)
  assert(get_hotcount(f1, countid_loops) == 1)

  f1(-1, 2)
  assert(get_hotcount(f1, countid_loop1) == lhot-1)
  assert(get_hotcount(f1, countid_loop2) == lhot-2)
  assert(get_hotcount(f1, countid_loops) == 3)  
end

function tests.func_backoff()
  local function f2() return 2 end
  local function f1(loopn)
    if loopn then
      local a = 0 
      -- Should abort root trace
      for i=1,loopn do a = a + 1 end
      return a
    else
      return 1
    end
  end
  
  calln(f1, fhot)
  assert(get_hotcount(f1, countid_func) == 0)
  assert(get_hotcount(f1, countid_calls) == fhot)
  
  jitlog.start()
  -- Trigger first trace attempt that aborts
  f1(20)

  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == fhot)
  
  -- Function hot count should of been reset with to the starting func_penalty hotvalue because the trace failed
  assert(get_hotcount(f1, countid_func) == func_penalty)
  -- a random offset is also added 
  assert(get_hotcount(f1, countid_func) < func_penalty+16)
  
  jitlog.reset()
  calln(f1, func_penalty)
  -- Should trigger a second trace attempt that aborts
  f1(20)
  
  -- Starting hotcount should of doubled with a small random backoff value added to it because the trace failed again.
  assert(get_hotcount(f1, countid_func) >= func_penalty*2)
  assert(get_hotcount(f1, countid_func) < func_penalty*2 + 16)
  
  result = parselog(jitlog.savetostring())
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == fhot + func_penalty + 1)
  assert(result.aborts[1].startpc == 0)
end

function tests.loop_backoff()
  local t = {a = 1, b = 2}
  local function f1(n, abort) 
    local a = 0 
    for i=1,n do 
      a = a + 1
      if abort then
        for _,_ in pairs(t) do end
      end
    end
    return a
  end 
  
  f1(lhot) 
  assert(get_hotcount(f1, countid_loop2) == 0)
  
  jitlog.start()
  -- Trigger first trace attempt that aborts
  f1(1)
  
  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == lhot)
  -- Loop hot count should of been reset with to the starting loop_penalty hotvalue because the trace failed
  assert(get_hotcount(f1, countid_loop2) == loop_penalty)
  
  f1(loop_penalty)
  assert(get_hotcount(f1, countid_loop2) == 0)
  
  jitlog.reset()
  -- Should trigger a second trace attempt that aborts
  f1(1)
  
  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == lhot + loop_penalty + 1)
  -- Starting hotcount should of doubled with a small random backoff value added to it because the trace failed again.
  assert(get_hotcount(f1, countid_loop2) >= loop_penalty*2)
  assert(get_hotcount(f1, countid_loop2) < loop_penalty*2 + 16)
end

function tests.loop_blacklist()
  local function f1(n) 
    local a = 0 
    for i=1,n do 
      a = a + 1
    end
    return a
  end
  
  jitlog.start()
  f1(lhot)
  assert(get_hotcount(f1, countid_loop1) == 0)
  f1(1)
  assert(get_hotcount(f1, countid_loop1) == loop_penalty)

  local prev = get_hotcount(f1, countid_loop1) 
  for i=1, maxattemps_loop-2 do
    local count = get_hotcount(f1, countid_loop1)
    prev = count
    f1(count)
    assert(get_hotcount(f1, countid_loop1) == 0)
    -- Trigger another trace abort
    f1(1)
    local newcount = get_hotcount(f1, countid_loop1)
    assert(newcount < countmax_loop)
    -- New starting count should be double the last + random(16)
    assert(newcount >= prev*2)
    assert(newcount <= (prev*2) + 16)
  end

  f1(get_hotcount(f1, countid_loop1))
  assert(get_hotcount(f1, countid_loop1) == 0)

  f1(1)
  -- Last abort should blacklist the function the count will be reset to its default starting value
  assert(get_hotcount(f1, countid_loop1) == lhot)

  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == maxattemps_loop, #result.aborts)
  assert(#result.proto_blacklist == 1)
  assert(result.proto_blacklist[1].bcindex ~= 0)
  assert(result.proto_blacklist[1].eventid == result.aborts[maxattemps_loop].eventid-1)
  assert(result.proto_blacklist[1].proto == result.aborts[maxattemps_loop].startpt)
end

function tests.func_blacklist()
  local function f1(loopn)
    if loopn then
      local a = 0 
      -- Should abort root trace
      for i=1,loopn do a = a + 1 end
      return a
    else
      return 1
    end
  end
  
  jitlog.start()
  calln(f1, fhot)
  assert(get_hotcount(f1, countid_func) == 0)
  f1(3)
  assert(get_hotcount(f1, countid_func) == func_penalty)

  local prev = get_hotcount(f1, countid_func) 
  for i=1, maxattemps_func-2 do
    local count = get_hotcount(f1, countid_func)
    prev = count
    calln(f1, count)
    assert(get_hotcount(f1, countid_func) == 0)
    -- Trigger another trace abort
    f1(3)
    local newcount = get_hotcount(f1, countid_func)
    assert(newcount < 50000)
    -- New starting count should be double the last + random(16)
    assert(newcount >= prev*2)
    assert(newcount <= (prev*2) + 16)
  end

  calln(f1, get_hotcount(f1, countid_func))
  assert(get_hotcount(f1, countid_func) == 0)

  f1(3)
  -- Last abort should blacklist the function the count will be reset to its default starting value
  assert(get_hotcount(f1, countid_func) == fhot)

  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == maxattemps_func, #result.aborts)
  assert(#result.proto_blacklist == 1)
  assert(result.proto_blacklist[1].bcindex == 0)
  assert(result.proto_blacklist[1].eventid == result.aborts[maxattemps_func].eventid-1)
  assert(result.proto_blacklist[1].proto == result.aborts[maxattemps_func].startpt)
end

local failed = false

for name, test in pairs(tests) do
  io.stdout:write("Running: "..name.."\n")
  if decoda_output then
    test()
  else
    local success, err = pcall(test)
    if not success then
      failed = true
      io.stderr:write("  FAILED ".. err.."\n")
    end
  end
  pcall(jitlog.shutdown)
end

if failed then
  -- Signal that we failed to travis
  os.exit(1)
end
