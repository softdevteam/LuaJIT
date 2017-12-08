local ffi = require"ffi"
require("table.new")
local format = string.format
local band = bit.band

local logdef = require("jitlog.reader_def")
local MsgType = logdef.MsgType
local msgnames = MsgType.names
local msgsizes = logdef.msgsizes

local base_actions = {}

function base_actions:stringmarker(buff)
  local msg = ffi.cast("MSG_stringmarker*", buff)
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

function logreader:processheader(header)
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
      assert(size < 255)
    end
  end

  local actionfuncs = self.actions or base_actions

  -- Map message functions associated with a message name to this files message types
  local dispatch = table.new(255, 0) 
  for i, name in ipairs(header.msgnames) do
    local action = actionfuncs[name]
    if action then
      dispatch[i-1] = action
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

local function makereader()
  local t = {
    eventid = 0,
    markers = {},
    verbose = false,
    logfilter = {
      --header = true,
      --stringmarker = true,
    }
  }
  return setmetatable(t, mt)
end

local lib = {
  makereader = makereader,
  parsebuffer = function(buff, length)
    local reader = makereader()
    assert(reader:parse_buffer(buff, length))
    return reader
  end, 
  parsefile = function(filepath)
    local reader = makereader()
    reader:parsefile(filepath)
    return reader
  end,
}

return lib
