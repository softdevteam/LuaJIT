
local readerlib = require("jitlog.reader")

local marker_counter = {
  init = function(self)
    self.marker_gcn = 0
    self.iterN = 0
    self.lastiterN = -1
  end,
  premsg = function(self, msgtype, size, pos)
    if self.msgnames[msgtype+1] == "gcstate" and self.lastiterN ~= self.iterN then
      self.lastiterN = self.iterN
      print("ITER:", self.iterN)
    end
  end,
  actions = {
    stringmarker = function(self, msg, marker)
      if marker.label == "BEGIN" then
        self.iterN = self.iterN + 1
      end
    end,
  }
}

local mixins = {
  marker_counter
}

reader = readerlib.makereader(mixins)
reader.logfilter.gcstate = true



data = reader:parsefile(logpath)