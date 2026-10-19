#!/usr/bin/env python3
"""Expected crosswalk bytes for ORCID's XML write samples.

    python3 -I scripts/crosswalk_expected.py SAMPLES_DIR OUT_DIR [--check]

For each `*.xml` in SAMPLES_DIR (reference/orcid-model/record_3.0/samples/
write_samples), writes OUT_DIR/<stem>.xml and OUT_DIR/<stem>.json: the bytes
yorc-rs writes for that sample as a document (`xml::to_string` /
`json::to_string` of what `xml::from_str` reads), which is what yorc-duckdb's writers must emit in
crosswalk mode (SET orcid_crosswalk = true) for the same item read back from
its rows. With --check, compares instead and exits 1 on a difference.

This is a model of yorc-rs's rules, not yorc-rs: the authoritative
comparison is the cross-implementation check (BENCHMARKS.md, "Parity"), which
compares yorc-duckdb's crosswalk output with the real Rust (and Zig) output. The rules, with their source:

- The element tree is the sample's, in document order, comments dropped,
  text trimmed of XML white space, `xsi:` attributes and namespace
  declarations dropped, elements with no text, attribute or child dropped.
  yorc-rs's tests/write_roundtrip.rs (`write_samples_round_trip`) asserts
  that its written document has exactly this canonical form
  (tests/support/mod.rs, `canonical`), children in order. No write sample
  carries a field ORCID sets itself (source, dates, path, display index),
  which yorc-duckdb's writers leave out.
- XML (src/xml/write.rs, module docs and the `Xml` emitter): the declaration
  and LF; two spaces per level; the prefix of each element is the last
  segment of its namespace URI (http://www.orcid.org/ns/<prefix>); every
  prefix used is declared on the root, before its attributes, in `xml::Ns`
  order (alphabetical); attributes put-code, path, visibility, display-index,
  then the element's own (`meta_attrs`); `<x/>` for an element without
  children or text; text escapes & < > and CR (&#13;), attributes also " TAB
  LF (`escape`); fuzzy-date parts as {:04} / {:02} (`opt_date`); a final LF.
- JSON (src/json.rs, the `w_*` functions and `to_string`): keys are local
  names, sorted (serde_json's BTreeMap), pretty-printed with two spaces and
  `": "`, a final LF; the {"value": ...} wrappers of `W::v` (VALUE below);
  `translated-title`, `amount` and a work's `country` as objects with
  "value" and their attribute; arrays for external ids, contributors, hosts'
  organizations and resource items (resource-items flattened into the
  research resource); bulk as {"bulk": [{"work": ...}]}; put-code a number.
  json.dumps(indent=2, sort_keys=True, ensure_ascii=False) prints a value as
  serde_json's to_string_pretty does (the same escapes; DEL raw).
"""

import json
import os
import sys
import xml.etree.ElementTree as ET

URI = "http://www.orcid.org/ns/"
XSI = "http://www.w3.org/2001/XMLSchema-instance"
XML_DECL = '<?xml version="1.0" encoding="UTF-8"?>\n'
WS = " \t\r\n"

# write.rs meta_attrs: the item attributes first, in this order
ATTR_ORDER = ("put-code", "path", "visibility", "display-index")

# json.rs W::v: text written as {"value": text} (prefix, local name)
VALUE = {
    ("common", "title"), ("common", "subtitle"), ("work", "journal-title"), ("common", "year"),
    ("common", "month"), ("common", "day"), ("common", "external-id-url"), ("common", "url"),
    ("work", "credit-name"), ("funding", "credit-name"), ("funding", "organization-defined-type"),
    ("peer-review", "review-url"), ("peer-review", "subject-url"), ("address", "country"),
    ("researcher-url", "url"),
}
# objects {"value": text, <attribute>: ...}: w_title (translated-title), w_amount, w_work's country
VALUE_WITH_ATTRS = {("common", "translated-title"), ("funding", "amount")}
# list items: (prefix, local name of the item) -> parent local names holding them as an array
ARRAYS = {
    ("common", "external-id"): {"external-ids", "review-identifiers"},
    ("work", "contributor"): {"contributors"},
    ("funding", "contributor"): {"contributors"},
    ("common", "organization"): {"hosts"},
}
# json.rs w_date: year {:04}, month and day {:02}
PADDED = {"year": 4, "month": 2, "day": 2}


class El:
    def __init__(self, prefix, name, attrs, text, kids):
        self.prefix, self.name, self.attrs, self.text, self.kids = prefix, name, attrs, text, kids


def split(tag):
    if not tag.startswith("{" + URI):
        raise ValueError("not an ORCID element: " + tag)
    uri, name = tag[1:].split("}", 1)
    return uri[len(URI):], name


def canonical(e):
    """The tree yorc-rs reads and writes back (None: an element it drops)."""
    prefix, name = split(e.tag)
    attrs = {}
    for k, v in e.attrib.items():
        if k.startswith("{" + XSI + "}"):
            continue
        if k.startswith("{"):
            raise ValueError("namespaced attribute " + k)
        attrs[k] = v.strip(WS)
    kids = [c for c in (canonical(k) for k in e) if c is not None]
    text = "" if kids else (e.text or "").strip(WS)
    if name in PADDED and prefix == "common" and text:
        text = "%0*d" % (PADDED[name], int(text))
    if not text and not attrs and not kids:
        return None
    return El(prefix, name, attrs, text, kids)


def esc(s, attr):
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\r", "&#13;")
    if attr:
        s = s.replace('"', "&quot;").replace("\t", "&#9;").replace("\n", "&#10;")
    return s


def prefixes(el, out):
    out.add(el.prefix)
    for k in el.kids:
        prefixes(k, out)
    return out


def ordered_attrs(attrs):
    first = [k for k in ATTR_ORDER if k in attrs]
    return first + [k for k in attrs if k not in ATTR_ORDER]


def xml_of(el, depth, out, root):
    qn = el.prefix + ":" + el.name
    out.append("  " * depth + "<" + qn)
    if root:
        for p in sorted(prefixes(el, set())):
            out.append(' xmlns:%s="%s%s"' % (p, URI, p))
    for k in ordered_attrs(el.attrs):
        out.append(' %s="%s"' % (k, esc(el.attrs[k], True)))
    if not el.kids:
        if el.text:
            out.append(">" + esc(el.text, False) + "</" + qn + ">\n")
        else:
            out.append("/>\n")
        return
    out.append(">\n")
    for k in el.kids:
        xml_of(k, depth + 1, out, False)
    out.append("  " * depth + "</" + qn + ">\n")


def attr_value(k, v):
    return int(v) if k == "put-code" else v


def json_of(el, parent):
    key = (el.prefix, el.name)
    if not el.kids:
        if key in VALUE_WITH_ATTRS or (key == ("common", "country") and parent == ("work", "work")):
            obj = {"value": el.text}
            for k, v in el.attrs.items():
                obj[k] = attr_value(k, v)
            return obj
        if el.attrs:
            raise ValueError("attributes on %s:%s, which json.rs writes bare" % key)
        return {"value": el.text} if key in VALUE else el.text
    obj = {}
    for k, v in el.attrs.items():
        obj[k] = attr_value(k, v)
    for kid in el.kids:
        kkey = (kid.prefix, kid.name)
        if kkey == ("research-resource", "resource-items"):
            # json.rs w_research_resource: "resource-item": [...] on the research resource
            obj.setdefault("resource-item", []).extend(json_of(i, kkey) for i in kid.kids)
            continue
        value = json_of(kid, key)
        if el.name in ARRAYS.get(kkey, ()):
            obj.setdefault(kid.name, []).append(value)
        elif kid.name in obj:
            raise ValueError("repeated %s:%s in %s:%s" % (kid.prefix, kid.name, el.prefix, el.name))
        else:
            obj[kid.name] = value
    return obj


def payloads(path):
    root = canonical(ET.parse(path).getroot())
    out = [XML_DECL]
    xml_of(root, 0, out, True)
    if (root.prefix, root.name) == ("bulk", "bulk"):
        value = {"bulk": [{"work": json_of(w, ("bulk", "bulk"))} for w in root.kids]}
    else:
        value = json_of(root, None)
    return "".join(out), json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    check = "--check" in argv[1:]
    if len(args) != 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    src, dst = args
    names = sorted(n for n in os.listdir(src) if n.endswith(".xml"))
    differ = 0
    for name in names:
        x, j = payloads(os.path.join(src, name))
        for ext, text in ((".xml", x), (".json", j)):
            path = os.path.join(dst, name[:-4] + ext)
            data = text.encode("utf-8")
            if check:
                try:
                    with open(path, "rb") as f:
                        same = f.read() == data
                except OSError:
                    same = False
                if not same:
                    print("differs: " + path, file=sys.stderr)
                    differ += 1
            else:
                os.makedirs(dst, exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
    print("%d samples, %s" % (len(names), ("%d files differ" % differ) if check else "written to " + dst),
          file=sys.stderr)
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
