#!/usr/bin/env python3
"""Escape must reach the application whenever there is no composition pending.

The plugin owns Escape only to cancel an in-progress pinyin composition. If it
started swallowing the idle Escape, applications would lose their own
close/cancel shortcut (chat windows, image previews, dialogs).
"""
import sys

from dbus_test_client import Client

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + (" " + detail if detail else ""), flush=True)


c = Client("wetype-escape")
c.focus()

c.press("esc")
c.commits.clear()
check("idle Escape is passed through, not consumed", not c.press("esc") and not c.commits)
c.commits.clear()
check("idle Escape release is passed through",
      not c.press("esc", release=True) and not c.commits)

# A composition owns exactly one Escape: it clears the preedit and nothing else.
c.type("nihao")
consumed = c.press("esc")
check("Escape while composing is consumed to clear the preedit", consumed)
c.commits.clear()
check("Escape after the preedit was cleared is passed through", not c.press("esc"))

# Punctuation state must not be entangled with the Escape path.
c.type("nihao")
c.press("space")
c.commits.clear()
check("Escape right after a commit is passed through", not c.press("esc") and not c.commits)

c.ic.FocusOut()
print("SUMMARY escape total=%d failed=%d" % (len(results), results.count(False)), flush=True)
print("E2E_RESULT =", "PASS" if all(results) else "FAIL", flush=True)
sys.exit(0 if all(results) else 1)
