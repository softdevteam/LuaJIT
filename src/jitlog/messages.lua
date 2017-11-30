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
    name = "gcstring",
    "address : GCRef",
    "len : u32",
    "hash : u32",
    "data : string[len]",
    structcopy = {
      fields = {
        "len",
        "hash",
      },
      arg = "s : GCstr *",
      store_address = "address",
    },
    use_msgsize = "len",
  },

  {
    name = "gcproto",
    "address : GCRef",
    "chunkname : GCRef",
    "firstline : i32",
    "numline : i32",
    "bcaddr : MRef",
    "bclen : u32",
    "bc : u32[bclen]",
    "sizekgc : u32",
    "kgc : GCRef[sizekgc]",
    "lineinfosize : u32",
    "lineinfo : u8[lineinfosize]",
    "varinfo_size : u32",
    "varinfo : u8[varinfo_size]",
    structcopy = {
      fields = {
        "chunkname",
        "firstline",
        "numline",
        bclen = "sizebc",
        "sizekgc",
      },
      arg = "pt : GCproto *",
      store_address = "address",
    },
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

  {
    name = "gcstate",
    "state : 8",
    "prevstate : 8",
    "totalmem : u32",
    "strnum : u32",
    "time : timestamp",
  },
}

return msgs
