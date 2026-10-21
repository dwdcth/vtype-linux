#!/usr/bin/env python3
"""Shift+letter must produce the capital letter in the Chinese input state."""
import sys

from dbus_test_client import Client

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + (" " + detail if detail else ""), flush=True)


c = Client("wetype-shiftupper")
c.focus()

# A real shifted key press is the uppercase keysym plus the Shift state bit.
c.commits.clear()
consumed = c.press("A", states=1)
check("Shift+A commits the capital instead of pinyin", consumed and c.commits == ["A"],
      "consumed=%s commits=%s" % (consumed, c.commits))

c.commits.clear()
for letter in ("D", "N", "A"):
    c.press(letter, states=1)
check("holding Shift across letters spells DNA", c.commits == ["D", "N", "A"], str(c.commits))

# Hello: the capital commits, the rest still behaves like pinyin input.
c.press("esc")
c.commits.clear()
c.press("H", states=1)
check("Shift+H commits H on its own", c.commits == ["H"], str(c.commits))
c.commits.clear()
consumed = c.press("e")
check("letters after a capital join the pinyin buffer", consumed and not c.commits,
      str(c.commits))
c.press("esc")

# Modifiers other than Shift keep their shortcut meaning.
c.commits.clear()
check("Ctrl+Shift+A stays a shortcut", not c.press("A", states=5) and not c.commits,
      str(c.commits))
check("Alt+Shift+A stays a shortcut", not c.press("A", states=8) and not c.commits,
      str(c.commits))
# CapsLock delivers an uppercase keysym without the Shift bit: pinyin unchanged.
check("CapsLock capital still becomes pinyin", c.press("A", states=0) and not c.commits,
      str(c.commits))
c.press("esc")

# Mid-composition shifting keeps the previous behaviour: no capital leak.
c.type("nihao")
c.commits.clear()
c.press("S", states=1)
check("Shift+S while composing does not leak a capital", not c.commits, str(c.commits))
c.press("space")
check("composition still resolves to one Chinese commit",
      len(c.commits) == 1 and c.commits[0] != "s", str(c.commits))

# Releases and the digit slot.
c.press("esc")
c.commits.clear()
check("Shift+A release stays passthrough",
      not c.press("A", states=1, release=True) and not c.commits, str(c.commits))
c.press("esc")
c.commits.clear()
c.press("1", states=1)
check("Shift with a digit is never treated as a capital", not c.commits, str(c.commits))

c.ic.FocusOut()
print("SUMMARY shiftupper total=%d failed=%d" % (len(results), results.count(False)), flush=True)
print("E2E_RESULT =", "PASS" if all(results) else "FAIL", flush=True)
sys.exit(0 if all(results) else 1)
