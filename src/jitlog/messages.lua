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
    "ggaddress : u64",
  },
  
  {
    name = "stringmarker",
    "time : timestamp",
    "flags : 16",
    "label : string",
    use_msgsize = "label",
  },
}

return msgs
