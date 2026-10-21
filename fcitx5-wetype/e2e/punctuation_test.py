#!/usr/bin/env python3
"""Punctuation width regression on the runner's private D-Bus session.

WeType is the Chinese input state, so owned punctuation is fullwidth no matter
what language precedes the cursor. The two exceptions that must stay ASCII are
a digit right before the cursor (3.14, 1,000) and structured fields.
"""
import sys

from dbus_test_client import Client, BASE_CAPABILITIES, paused_engine, pump

results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + (" " + detail if detail else ""), flush=True)


def raw(client, text):
    client.press("esc")
    client.commits.clear()
    client.type(text, wait=False)
    client.press("enter")
    check("Enter commits Latin text literally", client.commits == [text], str(client.commits))
    client.commits.clear()


first = Client("wetype-punctuation-A")
first.focus()
first.press(",")
check("fresh context is Chinese state, so comma is fullwidth", first.commits == ["，"],
      str(first.commits))
first.commits.clear()
raw(first, "hello")
first.press(".")
check("English word does not force a halfwidth period", first.commits == ["。"],
      str(first.commits))
first.commits.clear()

first.type("nihao")
first.press(",")
check("English history does not affect current Chinese candidate",
      first.commits == ["你好，"], str(first.commits))
first.commits.clear()
first.press(".")
check("standalone Chinese punctuation is fullwidth", first.commits == ["。"])
raw(first, "nihao")
first.press(",")
check("literal Latin commit still gets the Chinese comma", first.commits == ["，"],
      str(first.commits))
first.commits.clear()

first.type("nihao")
first.press("space")
first.commits.clear()
check("Ctrl-comma shortcut remains passthrough", not first.press(",", states=4) and not first.commits)
check("punctuation release remains passthrough", not first.press(",", release=True) and not first.commits)

# The widened table: every ASCII symbol is owned except the grave key.
for key, expected in [("?", "？"), ("!", "！"), (":", "："), (";", "；"), ("\\", "、"),
                      ("(", "（"), (")", "）"), ("[", "【"), ("]", "】"), ("{", "｛"),
                      ("}", "｝"), ("<", "《"), (">", "》"), ("-", "－"), ("_", "＿"),
                      ("+", "＋"), ("=", "＝"), ("*", "＊"), ("/", "／"), ("#", "＃"),
                      ("$", "＄"), ("%", "％"), ("&", "＆"), ("@", "＠"), ("^", "＾"),
                      ("~", "～"), ("|", "｜")]:
    first.commits.clear()
    consumed = first.press(key)
    check("Chinese state converts %s to fullwidth" % key,
          consumed and first.commits == [expected], str(first.commits))
# The grave key is the one symbol left alone, for markdown and shell quoting.
for key in ["`"]:
    first.commits.clear()
    consumed = first.press(key)
    check("unowned key %s stays passthrough" % key, not consumed and not first.commits,
          "consumed=%s commits=%s" % (consumed, first.commits))

# Quotes pair: the first press opens, the second closes, per input context.
first.press("esc")
first.commits.clear()
first.press('"')
first.press('"')
check("double quotes pair into Chinese quotes", first.commits == ["“", "”"],
      str(first.commits))
first.commits.clear()
first.press("'")
first.press("'")
check("single quotes pair into Chinese quotes", first.commits == ["‘", "’"],
      str(first.commits))
first.commits.clear()
first.press('"')
check("a lone quote opens", first.commits == ["“"], str(first.commits))
first.type("nihao")
first.commits.clear()
first.press('"')
check("quote while composing attaches the closing form to the candidate",
      first.commits and first.commits[0].endswith("”"), str(first.commits))
first.press("esc")

first.ic.FocusOut()
second = Client("wetype-punctuation-B")
second.focus()
second.press(",")
check("a second context has no history yet is still Chinese state",
      second.commits == ["，"], str(second.commits))
second.commits.clear()
raw(second, "hello")
second.ic.FocusOut()
first.focus()
first.commits.clear()
first.press(",")
check("returning to first context keeps its own state", first.commits == ["，"])
first.ic.FocusOut()

second.focus()
second.commits.clear()
second.press(".")
check("a returning context is still the Chinese input state", second.commits == ["。"],
      str(second.commits))

second.ic.FocusOut()
surrounding = Client("wetype-punctuation-surrounding", BASE_CAPABILITIES | (1 << 6))
surrounding.focus()
surrounding.surrounding("hello你好", 5)
surrounding.press(",")
check("Latin before the cursor no longer suppresses the Chinese comma",
      surrounding.commits == ["，"], str(surrounding.commits))
surrounding.commits.clear()
surrounding.surrounding("hello你好", 7)
surrounding.press(",")
check("cursor after Chinese word uses fullwidth", surrounding.commits == ["，"])
surrounding.commits.clear()
surrounding.surrounding("你好", 0)
surrounding.press(".")
check("cursor at beginning is empty context, still fullwidth",
      surrounding.commits == ["。"], str(surrounding.commits))
surrounding.commits.clear()
surrounding.surrounding("hello")
surrounding.type("nihao")
surrounding.press(",")
check("current Chinese candidate overrides stale surrounding text",
      surrounding.commits == ["你好，"], str(surrounding.commits))
surrounding.commits.clear()
surrounding.press(".")
check("latest commit wins until surrounding text is refreshed", surrounding.commits == ["。"])
surrounding.commits.clear()

# The digit exception, both with and without surrounding-text support.
surrounding.surrounding("版本 3.14")
check("digit before cursor keeps comma ASCII",
      not surrounding.press(",") and not surrounding.commits)
surrounding.surrounding("总共 1,000")
check("digit before cursor keeps period ASCII",
      not surrounding.press(".") and not surrounding.commits)
surrounding.surrounding("中文")
check("idle digits pass through", not surrounding.press("3"))
check("decimal period after a digit stays ASCII",
      not surrounding.press(".") and not surrounding.commits)
surrounding.surrounding("中文")
surrounding.commits.clear()
surrounding.press(0xFFB3)  # KP_3: the numpad row is where 3.14 is usually typed
check("numpad digit passes through", not surrounding.commits)
check("numpad digit also keeps the period ASCII",
      not surrounding.press(".") and not surrounding.commits)
surrounding.surrounding("中文 3 ")
surrounding.commits.clear()
check("trailing spaces do not cancel the digit exception",
      not surrounding.press(".") and not surrounding.commits)
surrounding.surrounding("第3名")
surrounding.commits.clear()
consumed = surrounding.press("?")
check("digit away from the cursor does not force ASCII",
      consumed and surrounding.commits == ["？"], str(surrounding.commits))
surrounding.surrounding("共 3")
surrounding.commits.clear()
consumed = surrounding.press("(")
check("the digit exception covers only . and , so brackets stay fullwidth",
      consumed and surrounding.commits == ["（"], str(surrounding.commits))

# A committed fullwidth symbol replaces the digit before the cursor, even when
# the client cannot provide surrounding text or has not refreshed it yet.
for with_surrounding in (False, True):
    for key, expected in ((".", "。"), (",", "，")):
        capabilities = BASE_CAPABILITIES | ((1 << 6) if with_surrounding else 0)
        trailing = Client("wetype-punctuation-trailing-digit", capabilities)
        trailing.focus()
        if with_surrounding:
            trailing.surrounding("3")
        else:
            assert not trailing.press("3"), "idle digit must pass through"
        trailing.press("?")
        consumed = trailing.press(key)
        check("fullwidth symbol clears digit exception for %s (%s)" %
              (key, "stale surrounding text" if with_surrounding else "no surrounding text"),
              consumed and trailing.commits == ["？", expected], str(trailing.commits))
        trailing.ic.FocusOut()

structured = Client("wetype-punctuation-email", BASE_CAPABILITIES | (1 << 7))
structured.focus()
structured.type("nihao")
structured.press(",")
check("structured Email field keeps ASCII separator", structured.commits == ["你好,"])
structured.commits.clear()
consumed = structured.press("?")
check("structured field keeps ASCII question mark", not consumed and not structured.commits,
      "consumed=%s commits=%s" % (consumed, structured.commits))
structured.commits.clear()
consumed = structured.press('"')
check("structured field keeps ASCII quote", not consumed and not structured.commits,
      "consumed=%s commits=%s" % (consumed, structured.commits))
structured.ic.FocusOut()

first.focus()
first.type("nihao")
first.press("space")
first.commits.clear()
# Ordinary IC with known Chinese history: no Email guard can mask a bug.
# No response can arrive during this case: the engine is deliberately paused.
with paused_engine():
    first.type("nihao", wait=False)
    first.press(",")
    check("pinyin literal fallback still gets the Chinese comma",
          first.commits == ["nihao，"], str(first.commits))
pump(0.5)
check("late responses do not add punctuation commits", first.commits == ["nihao，"])

print("SUMMARY punctuation total=%d failed=%d" % (len(results), results.count(False)), flush=True)
print("E2E_RESULT =", "PASS" if all(results) else "FAIL", flush=True)
sys.exit(0 if all(results) else 1)
