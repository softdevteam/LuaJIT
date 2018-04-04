local jit = require"jit"
local jit_util = require"jit.util"
local jitlog = require"jitlog"
local readerlib = require("jitlog.reader")
local get_hotcount = jit_util.funchcount
local format = string.format

local function hotscaling(start)
  local value_min = start
  local value_max = start
  local total_min = value_min
  local total_max = value_max
  for i=1, 32 do
    print("abort:", i, "=", (value_min/2).."-"..(value_max/2))
    value_min = (value_min*2)
    value_max = (value_max*2) + 15
    total_min = total_min + value_min
    total_max = total_max + value_max
    if value_min > 50000 then
      break
    end
  end
  print("min calls to get blacklisted:", total_min)
  print("max calls to get blacklisted:", total_max)
end

hotscaling(36)

hotscaling(36*2)

local function printinfo(f, label, loops)
  label = label or "hotcounters: "
  loops = loops or 0
  for i=1, loops do
    print("loophc:", get_hotcount(f, i))
  end
  print(format("%s hot %d calls %d, loops %d", label, get_hotcount(f, 0), get_hotcount(f, countid_calls), get_hotcount(f, -2)))
end

local function parselog(log, verbose)
  local result = readerlib.makereader()
  if verbose then
    result.verbose = true
  end
  assert(result:parse_buffer(log, #log))
  return result
end

local tests = {}

local fhot, lhot
local func_penalty, loop_penalty

if seperate_counters then
  fhot = (56*2) -1
  lhot = 56 -1
  func_penalty = 36*2
  loop_penalty = 36
else
  fhot = (56*2) -1
  lhot = (56*2) -1
  func_penalty = 36*2
  loop_penalty = 36*2
end

local countid_calls = -1
local countid_loops = -2 -- total number of times a loop
local countid_func = 0
local countid_loop1 = 1 -- The value of first LOOPHC bytecode in the function
local countid_loop2 = 2

local function calln(f, n)
  for i=1, n do
    f()
  end
end

jit.off(calln)

function tests.func_hotcounters()
  local function f1() end
  jit.off()
  jit.on()
  
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
  
  jit.off()
  jit.on()
  
  local function f2() end
  calln(f2, fhot)
  assert(get_hotcount(f2, countid_calls) == fhot, get_hotcount(f2, countid_calls))
  assert(get_hotcount(f2, countid_func) == 0, get_hotcount(f2, countid_func) )
  f2()
  assert(get_hotcount(f2, countid_calls) == fhot+1, get_hotcount(f2, countid_calls))
  assert(get_hotcount(f2, countid_func) == fhot, get_hotcount(f2, countid_func))
  -- Functinon JIT'ed now
  f2()
  -- Call counter still goes up for JFUNC
  assert(get_hotcount(f2, countid_calls) == fhot+2, get_hotcount(f2, countid_calls))
  -- But the function hot counter should stop changing now its JIT'ed
  assert(get_hotcount(f2, countid_func) == fhot, get_hotcount(f2, countid_func))
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
  assert(get_hotcount(f1, countid_calls) == 1) -- call count should of incremented
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
end

function tests.multiloop_hotcounters()
  jit.off()
  jit.on()

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
  jit.off()
  jit.on()
  
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

  -- Function hot count should of been reset with to the starting func_penalty hotvalue because the trace failed
  assert(get_hotcount(f1, countid_func) == func_penalty)
  
    -- a random offset is also added 
  assert(get_hotcount(f1, countid_func) < func_penalty+16)

  local result1 = jitlog.savetostring()
  jitlog.reset()
  calln(f1, func_penalty)
  -- Should trigger a second trace attempt that aborts
  f1(20)
  
  local result2 = jitlog.savetostring()
  -- Starting hotcount should of doubled with a small random backoff value added to it because the trace failed again.
  assert(get_hotcount(f1, countid_func) >= func_penalty*2)
  assert(get_hotcount(f1, countid_func) < func_penalty*2 + 16)
  
  
  local result = parselog(result1)
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == fhot)
  
  result = parselog(result2)
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == fhot + func_penalty + 1)
end

function tests.loop_backoff()
  jit.off()
  jit.on()
  
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
  
  f1(55) 
  assert(get_hotcount(f1, countid_loop2) == 1)
  
  jitlog.start()
  -- Trigger first trace attempt that aborts
  f1(1)
  
   -- Loop hot count should of been reset with to the starting loop_penalty hotvalue because the trace failed
  assert(get_hotcount(f1, countid_loop2) == loop_penalty)

  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == 55)
 
  f1(loop_penalty/2)
  assert(get_hotcount(f1, countid_loop2) == 0)
  
  jitlog.reset()
  -- Should trigger a second trace attempt that aborts
  f1(1)

  -- Starting hotcount should of doubled with a small random backoff value added to it because the trace failed again.
  assert(get_hotcount(f1, countid_loop2) >= loop_penalty*2)
  assert(get_hotcount(f1, countid_loop2) < loop_penalty*2 + 16)
  
  local result = parselog(jitlog.savetostring())
  assert(#result.aborts == 1)
  assert(result.aborts[1].callcount == (lhot + loop_penalty + 1)/2)
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

io.stdin:read()

if failed then
  -- Signal that we failed to travis
  os.exit(1)
end
