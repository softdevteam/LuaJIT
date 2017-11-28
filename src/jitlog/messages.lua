local msgs = {
  {
    name = "header",
    "version : u32",
    "flags : u32",
    "headersize: u32",
    "msgtype_count :  u8",
    "msgsizes : i32[msgtype_count]",
    "msgnames_length : u32",
    "msgnames : stringlist[msgnames_length]",
    "cpumodel_length : u8",
    "cpumodel : string[cpumodel_length]",
    "os : string",
    "starttime : timestamp",
    "ggaddress : u64",
  },
  
  {
    name = "stringmarker",
    "flags : 16",
    "label : string",
    "time : timestamp",
    use_msgsize = "label",
  },

  {
    name = "traceexit",
    "isgcexit : bool",
    "traceid : 14",
    "exit : 9",
  },

  {
    name = "traceexit_large",
    "isgcexit : bool",
    "traceid : u16",
    "exit : u16",
  },

  {
    name = "alltraceflush",
    "reason : u16",
    "time : timestamp",
    "tracelimit : u16",
    "mcodelimit : u32",
  },
}

return msgs
