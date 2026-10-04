#!/usr/bin/env python3
"""Regenerate the web flasher's TEACHABLE intent list from the firmware's real
command table (so the Teach panel never offers a phrase the fox doesn't know)."""
import re, json, os
R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
m = open(os.path.join(R, "firmware/main/fox_main.cpp")).read()
cmds = re.findall(r'\{(\d+),\s+"([^"]+)",\s+"([^"]+)"\}',
                  m[m.index("static const Command COMMANDS[]"):m.index("static const size_t COMMAND_COUNT")])
t = open(os.path.join(R, "firmware/main/fox_teach.inc")).read()
s = t.index("static bool teach_is_logic"); logic = set(re.findall(r'"([ct]_[a-z_]+)"', t[s:t.index("return false;", s)]))
items = [{"id": a, "say": p.split(";")[0]} for _, p, a in cmds if a.startswith(("t_", "c_")) and a not in logic]
p = os.path.join(R, "web/index.html"); w = open(p).read()
w2 = re.sub(r"/\*TEACHABLE_BEGIN\*/.*?/\*TEACHABLE_END\*/",
            "/*TEACHABLE_BEGIN*/const TEACHABLE=" + json.dumps(items) + ";/*TEACHABLE_END*/", w, flags=re.S)
open(p, "w").write(w2)
print(f"gen_teachable: {len(items)} teachable intents ({'updated' if w2 != w else 'unchanged'})")
