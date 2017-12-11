local ffi = require"ffi"
require("table.new")
local format = string.format
local band = bit.band

local logdef = require("jitlog.reader_def")
local MsgType = logdef.MsgType
local msgnames = MsgType.names
local msgsizes = logdef.msgsizes

local base_actions = {}

function base_actions:stringmarker(msg)
  local label = msg:get_label()
  local flags = msg:get_flags()
  local time = msg.time
  local marker = {
    label = label,
    time = time,
    eventid = self.eventid,
    flags = flags,
    type = "string"
  }
  self.markers[#self.markers + 1] = marker
  self:log_msg("stringmarker", "StringMarker: %s %s", label, time)
  return marker
end

function base_actions:traceexit(msg)
  local id = msg:get_traceid()
  local exit = msg:get_exit()
  local gcexit = msg:get_isgcexit()
  self.exits = self.exits + 1
  if gcexit then
    assert(self.gcstate == "atomic" or self.gcstate == "finalize")
    self.gcexits = self.gcexits + 1
    self:log_msg("traceexit", "TraceExit(%d): %d GC Triggered", id, exit)
  else
    self:log_msg("traceexit", "TraceExit(%d): %d", id, exit)
  end
  return id, exit, gcexit
end

local flush_reason =  {
  [0] = "other",
  "user_requested",
  "max_mcode",
  "max_trace",
  "profiletoggle",
  "set_builtinmt",
  "set_immutableuv",
}

function base_actions:alltraceflush(msg)
  local reason = msg:get_reason()
  local flush = {
    reason = flush_reason[reason],
    eventid = self.eventid,
    time = msg.time,
    maxmcode = msg.mcodelimit,
    maxtrace = msg.tracelimit,
  }
  self.flushes[#self.flushes + 1] = flush
  self:log_msg("alltraceflush", "TraceFlush: Reason '%s', maxmcode %d, maxtrace %d", flush.reason, msg.mcodelimit, msg.tracelimit)
  return flush
end

local gcstates  = {
  [0] = "pause", 
  "propagate", 
  "atomic",
  "sweepstring", 
  "sweep", 
  "finalize",
}

function base_actions:gcstate(msg)
  local newstate = msg:get_state()
  local prevstate = msg:get_prevstate()
  local oldstate = self.gcstateid
  self.gcstateid = newstate
  self.gcstate = gcstates[newstate]
  self.gcstatecount = self.gcstatecount + 1
  
  if oldstate ~= newstate then
    -- A new GC cycle has only started once we're past the 'pause' GC state 
    if oldstate == nil or newstate == 1 or (oldstate > newstate and newstate > 0)  then
      self.gccount = self.gccount + 1
    end
    self:log_msg("gcstate", "GCState(%s): changed from %s", newstate, oldstate)
  end
  
  self.peakmem = math.max(self.peakmem or 0, msg.totalmem)
  self.peakstrnum = math.max(self.peakstrnum or 0, msg.strnum)
  self:log_msg("gcstate", "GCStateStats: MemTotal = %dMB, StrCount = %d", msg.totalmem/(1024*1024), msg.strnum)
  return self.gcstate, gcstates[prevstate]
end

local logreader = {}

function logreader:log(fmt, ...)
  if self.verbose then
    print(format(fmt, ...))
  end
end

function logreader:log_msg(msgname, fmt, ...)
  if self.verbose or self.logfilter[msgname] then
    print(format(fmt, ...))
  end
end

function logreader:readheader(buff, buffsize, info)
  local header = ffi.cast("MSG_header*", buff)
  
  local msgtype = band(header.header, 0xff)  
  if msgtype ~= 0 then
    return false, "bad header msg type"
  end
  
  if header.msgsize > buffsize then
    return false, "bad header vsize"
  end
  
  if header.headersize > header.msgsize then
    return false, "bad fixed header size"
  end
  
  if header.headersize ~= -msgsizes[MsgType.header + 1] then
    self:log_msg("header", "Warning: Message header fixed size does not match our size")
  end
  
  info.version = header.version
  info.size = header.msgsize
  info.fixedsize = header.headersize
  info.os = header:get_os()
  info.cpumodel = header:get_cpumodel()
  info.starttime = header.starttime
  self:log_msg("header", "LogHeader: Version %d, OS %s, CPU %s", info.version, info.os, info.cpumodel)

  local file_msgnames = header:get_msgnames()
  info.msgnames = file_msgnames
  self:log_msg("header", "  MsgTypes: %s", table.concat(file_msgnames, ", "))
  
  local sizearray = header:get_msgsizes()
  local msgtype_count = header:get_msgtype_count()
  local file_msgsizes = {}
  for i = 1, msgtype_count do
    file_msgsizes[i] = sizearray[i-1]
  end
  info.msgsizes = file_msgsizes
 
  if msgtype_count ~= MsgType.MAX then
    self:log_msg("header", "Warning: Message type count differs from known types")
  end
    
  return true
end
  
function logreader:parsefile(path)
  local logfile, msg = io.open(path, "rb")
  if not logfile then
    error("Error while opening jitlog '"..msg.."'")
  end
  local logbuffer = logfile:read("*all")
  logfile:close()

  self:parse_buffer(logbuffer, #logbuffer)
end

local function make_msgparser(file_msgsizes, dispatch, aftermsg)
  local msgtype_max = #file_msgsizes
  local msgsize = ffi.new("uint8_t[256]", 0)
  for i, size in ipairs(file_msgsizes) do
    if size < 0 then
      size = 0
    else
      assert(size < 255)
    end
    msgsize[i-1] = size
  end

  return function(self, buff, length, partial)
    local pos = ffi.cast("char*", buff)
    local buffend = pos + length

    while pos < buffend do
      local header = ffi.cast("uint32_t*", pos)[0]
      local msgtype = band(header, 0xff)
      -- We should never see the header mesaage type here so msgtype > 0
      assert(msgtype > 0 and msgtype < msgtype_max, "bad message type")
      
      local size = msgsize[msgtype]
      -- If the message was variable length read its total size field out of the buffer
      if size == 0 then
        size = ffi.cast("uint32_t*", pos)[1]
        assert(size >= 8, "bad variable length message")
      end
      if size > buffend - pos then
        if partial then
          break
        else
          error("bad message size")
        end
      end

      local action = dispatch[msgtype]
      if action then
        action(self, pos, size)
      end
      aftermsg(self, msgtype, size, pos)
      
      self.eventid = self.eventid + 1
      pos = pos + size 
    end
      
    return pos
  end
end

local function nop() end

local function make_msghandler(msgname, base, funcs)
  -- See if we can go for the simple case with no extra funcs call first
  if not funcs or (type(funcs) == "table" and #funcs == 0) then
    return function(self, buff, limit)
      base(self, ffi.cast(msgname.."*", buff), limit)
      return
    end
  elseif type(funcs) == "function" or #funcs == 1 then
    local f = (type(funcs) == "function" and funcs) or funcs[1]
    return function(self, buff, limit)
      local msg = ffi.cast(msgname, buff)
      f(self, msg, base(self, msg, limit))
      return
    end
  else
    return function(self, buff, limit)
      local msg = ffi.cast(msgname, buff)
      local ret1, ret2 = base(msg, limit)
      for _, f in ipairs(funcs) do
        f(self, msg, ret1, ret2)
      end
    end
  end
end

function logreader:processheader(header)
  self.starttime = header.starttime

  -- Make the msgtype enum for this file
  local msgtype = {
    names = header.msgnames
  } 
  self.msgtype = msgtype
  self.msgnames = header.msgnames
  for i, name in ipairs(header.msgnames) do
    msgtype[name] = i-1
  end
  
  for _, name in ipairs(msgnames) do
    if not msgtype[name] and name ~= "MAX" then
       self:log_msg("header", "Warning: Log is missing message type ".. name)
    end
  end
  
  self.msgsizes = header.msgsizes
  for i, size in ipairs(header.msgsizes) do
    local name = header.msgnames[i]
    local id = MsgType[name]
    if id and msgsizes[id + 1] ~= size then
      local oursize = math.abs(msgsizes[id + 1])
      local logs_size = math.abs(size)
      if logs_size < oursize then
        error(format("Message type %s size %d is smaller than an expected size of %d", name, logs_size, oursize))
      else
        self:log_msg("header", "Warning: Message type %s is larger than ours %d vs %d", name, oursize, logs_size)
      end
    end
    if size < 0 then
      assert(-size < 4*1024)
    else
      -- Msg size dispatch table is designed to be only 8 bits per slot
      assert(size < 255 and size >= 4)
    end
  end

  -- Map message functions associated with a message name to this files message types
  local dispatch = table.new(255, 0)
  for i = 0, 254 do
    dispatch[i] = nop
  end
  local base_actions = self.base_actions or base_actions
  for i, name in ipairs(header.msgnames) do
    local action = self.actions[name]
    if base_actions[name] or action then
      dispatch[i-1] = make_msghandler("MSG_"..name, base_actions[name], action)
    end
  end
  self.dispatch = dispatch
  self.header = header
  
  self.parsemsgs = make_msgparser(self.msgsizes, dispatch, self.allmsgcb or nop)
end

function logreader:parse_buffer(buff, length)
  buff = ffi.cast("char*", buff)

  if not self.seenheader then
    local header = {}
    self.seenheader = true
    local success, errmsg = self:readheader(buff, length, header)
    if not success then
      return false, errmsg
    end
    self:processheader(header)
    buff = buff + self.header.size
  end

  self:parsemsgs(buff, length - self.header.size)
  return true
end

local mt = {__index = logreader}

local function applymixin(self, mixin)
  if mixin.init then
    mixin.init(self)
  end
  if mixin.actions then
    for name, action in pairs(mixin.actions) do
      local list = self.actions[name] 
      if not list then
        self.actions[name] = {action}
      else
        list[#list + 1] = action
      end
    end
  end
  if mixin.aftermsg then
    if not self.allmsgcb then
      self.allmsgcb = mixin.aftermsg
    else
      local callback1 = self.allmsgcb
      local callback2 = mixin.aftermsg
      self.allmsgcb = function(self, msgtype, size, pos)
        callback1(self, msgtype, size, pos)
        callback2(self, msgtype, size, pos)
      end
    end
  end
end

local msgstats_mixin = {
  init = function(self)
    local datatotals = table.new(255, 0)
    for i = 0, 255 do
      datatotals[i] = 0
    end
    self.datatotals = datatotals
  
    local msgcounts = table.new(255, 0)
    for i = 0, 255 do
      msgcounts[i] = 0
    end
    self.msgcounts = msgcounts
  end,
  aftermsg = function(self, msgtype, size, pos)
    self.datatotals[msgtype] = self.datatotals[msgtype] + size
    self.msgcounts[msgtype] = self.msgcounts[msgtype] + 1
  end,
}

local builtin_mixins = {
  msgstats = msgstats_mixin,
}

local function makereader(mixins)
  local t = {
    eventid = 0,
    actions = {},
    markers = {},
    flushes = {},
    exits = 0,
    gcexits = 0, -- number of trace exits force triggered by the GC being in the 'atomic' or 'finalize' states
    gccount = 0, -- number GC full cycles that have been seen in the log
    gcstatecount = 0, -- number times the gcstate changed
    verbose = false,
    logfilter = {
      --header = true,
      --stringmarker = true,
    }
  }
  if mixins then
    for _, mixin in ipairs(mixins) do
      applymixin(t, mixin)
    end
  end
  return setmetatable(t, mt)
end

local lib = {
  makereader = makereader,
  parsebuffer = function(buff, length)
    if not length then
      length = #buff
    end
    local reader = makereader()
    assert(reader:parse_buffer(buff, length))
    return reader
  end, 
  parsefile = function(filepath)
    local reader = makereader()
    reader:parsefile(filepath)
    return reader
  end,
  base_actions = base_actions,
  make_msgparser = make_msgparser,
  mixins = builtin_mixins,
}

return lib
