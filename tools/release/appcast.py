#!/usr/bin/env python3
"""The Sparkle appcast (appcast.xml) of a release: tools/release.sh runs it; nothing here is app-specific.

  appcast.py add   OLD NEW --title T --link L --version V --build B --url U --length N --signature S --min-os M --notes TEXT
  appcast.py check FILE    --build B --url U --length N --signature S

add:   writes NEW = OLD with a new <item> on top (OLD may be missing: a first appcast, with no items yet). Older items
       are kept as they are, so a Mac that skipped a release still finds the newest. Refuses a build already listed,
       or one not higher than the newest item's.
check: the newest item of FILE (e.g. the live appcast) must be exactly this build, URL, length and signature.

Each item: title, pubDate, sparkle:version (the build number, what Sparkle compares), sparkle:shortVersionString,
sparkle:minimumSystemVersion, the notes as HTML, and the enclosure with its length and sparkle:edSignature
(sign_update's EdDSA signature of the zip). Plain Python, standard library only.
"""
import argparse
import email.utils
import html
import sys
import xml.etree.ElementTree as ET

SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
NS = {"sparkle": SPARKLE}


def first_item(text):
    channel = ET.fromstring(text).find("channel")
    if channel is None:
        sys.exit("appcast: no <channel>")
    return channel, channel.findall("item")


def item_build(item):
    v = item.find("sparkle:version", NS)
    if v is None or not (v.text or "").strip():
        enc = item.find("enclosure")
        return (enc.get("{%s}version" % SPARKLE) if enc is not None else None) or ""
    return v.text.strip()


def add(a):
    try:
        old = open(a.old, encoding="utf-8").read()
    except FileNotFoundError:
        old = ""
    if not old.strip():
        old = ('<?xml version="1.0" encoding="utf-8"?>\n'
               '<rss version="2.0" xmlns:sparkle="%s" xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
               '<channel>\n'
               '<title>%s</title>\n'
               '<link>%s</link>\n'
               '<description>%s updates for the Mac</description>\n'
               '<language>en</language>\n'
               '</channel>\n'
               '</rss>\n') % (SPARKLE, html.escape(a.title), html.escape(a.link), html.escape(a.title))
    _, items = first_item(old)
    builds = [item_build(i) for i in items]
    if a.build in builds:
        sys.exit("appcast: build %s is already in the appcast" % a.build)
    if builds and builds[0].isdigit() and int(a.build) <= int(builds[0]):
        sys.exit("appcast: build %s is not higher than the newest item's (%s)" % (a.build, builds[0]))
    notes = "".join("<p>%s</p>" % html.escape(line) for line in a.notes.splitlines() if line.strip())
    item = ("<item>\n"
            "<title>Version %s</title>\n"
            "<pubDate>%s</pubDate>\n"
            "<sparkle:version>%s</sparkle:version>\n"
            "<sparkle:shortVersionString>%s</sparkle:shortVersionString>\n"
            "<sparkle:minimumSystemVersion>%s</sparkle:minimumSystemVersion>\n"
            "<description><![CDATA[%s]]></description>\n"
            '<enclosure url="%s" length="%s" type="application/octet-stream" sparkle:edSignature="%s"/>\n'
            "</item>\n") % (html.escape(a.version), email.utils.formatdate(localtime=True), html.escape(a.build),
                            html.escape(a.version), html.escape(a.min_os), notes, html.escape(a.url, quote=True),
                            int(a.length), html.escape(a.signature, quote=True))
    # on top: before the first <item>, or before </channel> in a first appcast (text, so the rest stays as it was)
    at = old.find("<item>")
    if at < 0:
        at = old.rindex("</channel>")
    new = old[:at] + item + old[at:]
    _, items = first_item(new)   # parses, and the new item is the first
    if item_build(items[0]) != a.build or len(items) != len(builds) + 1:
        sys.exit("appcast: the new item did not land on top")
    open(a.new, "w", encoding="utf-8").write(new)


def check(a):
    _, items = first_item(open(a.file, encoding="utf-8").read())
    if not items:
        sys.exit("appcast: %s has no items" % a.file)
    i = items[0]
    enc = i.find("enclosure")
    got = (item_build(i), enc.get("url"), enc.get("length"), enc.get("{%s}edSignature" % SPARKLE))
    want = (a.build, a.url, str(int(a.length)), a.signature)
    if got != want:
        sys.exit("appcast: newest item is %s, expected %s" % (got, want))
    print("appcast: newest item is build %s, %s bytes, signed; %d item(s) in all" % (a.build, a.length, len(items)))


p = argparse.ArgumentParser()
sub = p.add_subparsers(dest="cmd", required=True)
pa = sub.add_parser("add")
pa.add_argument("old"); pa.add_argument("new")
for k in ("title", "link", "version", "notes", "min_os"):
    pa.add_argument("--" + k.replace("_", "-"), dest=k, required=True)
pc = sub.add_parser("check")
pc.add_argument("file")
for sp in (pa, pc):
    for k in ("build", "url", "length", "signature"):
        sp.add_argument("--" + k, required=True)
a = p.parse_args()
add(a) if a.cmd == "add" else check(a)
