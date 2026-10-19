"""XSD name coverage of yorc-duckdb's sections and writers (docs/COVERAGE.md).

Reads the vendored ORCID message schema (reference/orcid-model/**/*.xsd), the
extraction tables in src/core/sections.cpp and the writer templates in
src/core/writer.cpp, and prints the coverage totals and one markdown row per
declared element/attribute name: which section columns carry it, or whether
it is a structural container, or why it is not mapped; how it appears in
ORCID's JSON (src/core/json_map.cpp); and whether a writer writes it.
Python 3 standard library only; reads files, writes stdout.

    python3 audit/tools/xsd_coverage.py .            # from the repository root
    python3 audit/tools/xsd_coverage.py . --json     # the raw tabulation
"""

import glob
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
XS = "{http://www.w3.org/2001/XMLSchema}"

# ---------------------------------------------------------------- XSDs
types = {}  # (ns, name) -> ('complex'|'simple', node, file, nsmap)
decls = []  # dicts: kind, name, ns, file, parent, typeref, node, nsmap

def nsmap_of(path):
    m = {}
    for ev, (p, uri) in ET.iterparse(path, events=["start-ns"]):
        m[p] = uri
    return m

files = sorted(glob.glob(os.path.join(ROOT, "reference/orcid-model/**/*.xsd"), recursive=True))
trees = {}
for f in files:
    nsm = nsmap_of(f)
    tree = ET.parse(f)
    root = tree.getroot()
    tns = root.get("targetNamespace")
    trees[f] = (root, nsm, tns)
    for tag, kind in ((XS + "complexType", "complex"), (XS + "simpleType", "simple")):
        for t in root.iter(tag):
            if t.get("name"):
                types[(tns, t.get("name"))] = (kind, t, f, nsm)

def qname(value, nsm, tns):
    if value is None:
        return None
    if ":" in value:
        p, n = value.split(":", 1)
        return (nsm.get(p), n)
    return (nsm.get("", tns), value)

def walk(node, f, nsm, tns, context):
    for child in node:
        tag = child.tag
        if tag == XS + "complexType" and child.get("name"):
            walk(child, f, nsm, tns, "type " + child.get("name"))
            continue
        if tag in (XS + "element", XS + "attribute"):
            kind = "element" if tag == XS + "element" else "attribute"
            if child.get("name"):
                decls.append(dict(kind=kind, name=child.get("name"), ns=tns, file=os.path.relpath(f, os.path.join(ROOT, "reference/orcid-model")),
                                  parent=context, type=child.get("type"), node=child, nsm=nsm, tns=tns))
                walk(child, f, nsm, tns, kind + " " + child.get("name"))
            continue
        walk(child, f, nsm, tns, context)

for f, (root, nsm, tns) in trees.items():
    walk(root, f, nsm, tns, "schema")

def classify_complex(ct, nsm, tns, depth=0):
    """'data' when the type carries text (simpleContent, mixed, or extends a data type), else 'structural'."""
    if ct.get("mixed") == "true":
        return "data"
    if ct.find(XS + "simpleContent") is not None:
        return "data"
    cc = ct.find(XS + "complexContent")
    if cc is not None:
        for d in cc:
            base = qname(d.get("base"), nsm, tns)
            if base in types and depth < 10:
                k, n, f2, nsm2 = types[base]
                if k == "simple":
                    return "data"
                if classify_complex(n, nsm2, base[0], depth + 1) == "data":
                    has_children = any(x.tag in (XS + "sequence", XS + "choice", XS + "all") for x in d)
                    return "data" if not has_children else "structural"
        return "structural"
    if any(x.tag in (XS + "sequence", XS + "choice", XS + "all", XS + "group") for x in ct):
        return "structural"
    # attributes only, no content: an empty element (data lives in its attributes)
    return "empty"

def classify(d):
    if d["kind"] == "attribute":
        return "data"
    n = d["node"]
    inline_ct = n.find(XS + "complexType")
    if inline_ct is not None:
        return classify_complex(inline_ct, d["nsm"], d["tns"])
    if n.find(XS + "simpleType") is not None:
        return "data"
    t = d["type"]
    if t is None:
        return "data"
    q = qname(t, d["nsm"], d["tns"])
    if q[0] == "http://www.w3.org/2001/XMLSchema":
        return "data"
    if q in types:
        k, node, f2, nsm2 = types[q]
        return "data" if k == "simple" else classify_complex(node, nsm2, q[0])
    return "unknown:" + t

for d in decls:
    d["class"] = classify(d)

# ---------------------------------------------------------------- sections.cpp
src = open(os.path.join(ROOT, "src/core/sections.cpp")).read()
cols = re.findall(r'\{"(\w+)", "(\w+)", [VIBL], "([^"]*)"\}', src)
items = re.findall(r'\{"(\w+)", "(\^?[a-z/-]+)"\}', src)

leaf = {}       # name -> set of section.column (element leaf)
attr = {}       # name -> set of section.column (attribute)
inter = {}      # name -> set of section.column (intermediate path step)
for sec, col, path in cols:
    if path.startswith("$"):
        continue
    for alt in path.split("|"):
        alt = alt.lstrip("*#^")
        elems, at = (alt.split("@", 1) + [None])[:2]
        parts = [p for p in elems.split("/") if p]
        if at:
            a = at.split(":")[-1]
            attr.setdefault(a, set()).add(f"{sec}.{col}")
            for p in parts:
                inter.setdefault(p, set()).add(f"{sec}.{col}")
        else:
            for p in parts[:-1]:
                inter.setdefault(p, set()).add(f"{sec}.{col}")
            if parts:
                leaf.setdefault(parts[-1], set()).add(f"{sec}.{col}")
# ---------------------------------------------------------------- writer.cpp
wsrc = open(os.path.join(ROOT, "src/core/writer.cpp")).read()
# element names: the second argument of the template builders (the first is the prefix)
written = set(re.findall(r'\b(?:Leaf|Group|Repeat|Root|Organization|FuzzyDate)\([^,()]+, "([\w-]+)"', wsrc))
# the seven affiliation document elements (Root(type, type, ...)) and the bulk element
m = re.search(r'kAffiliationTypes\[\] = \{([^}]*)\}', wsrc)
if m:
    written |= set(re.findall(r'"([\w-]+)"', m.group(1)))
if 'bulk.name = "bulk"' in wsrc:
    written.add("bulk")
written_attrs = set(re.findall(r'\{"([\w:-]+)", [^,{}]+, (?:INTEGER|REQUIRED|0)\}', wsrc))

item_names = {}
for sec, pat in items:
    for p in pat.lstrip("^").split("/"):
        item_names.setdefault(p, set()).add(sec)

# computed columns ($specials) read these names directly (sections.cpp Extractor)
special = {
    ("attribute", "path"): ["every record section .orcid (root @path)", "contributors/external_ids .orcid fallback (item @path)"],
    ("attribute", "put-code"): ["*.owner_put_code, *.owner (nearest ancestor with @put-code)"],
    ("element", "orcid-identifier"): ["*.orcid fallback (orcid-identifier/path)"],
    ("element", "external-id-type"): ["works.doi ($doi: type doi + relationship self)"],
    ("element", "external-id-value"): ["works.doi"],
    ("element", "external-id-relationship"): ["works.doi"],
    ("element", "group"): ["*.group_index ($group)"],
    ("element", "affiliation-group"): ["affiliations.group_index ($group)"],
    ("element", "developer-message"): ["errors.orcid / errors.primary_orcid (iD in the message)"],
    ("element", "user-message"): ["errors.orcid / errors.primary_orcid (iD in the message)"],
}

out = []
names = {}
for d in decls:
    key = (d["kind"], d["name"])
    e = names.setdefault(key, dict(kind=d["kind"], name=d["name"], files=set(), classes=set(), parents=set()))
    e["files"].add(d["file"])
    e["classes"].add(d["class"])
    e["parents"].add(d["parent"])

for (kind, name), e in sorted(names.items(), key=lambda kv: (kv[0][1], kv[0][0])):
    if kind == "attribute":
        carried = sorted(attr.get(name, set()))
    else:
        carried = sorted(leaf.get(name, set()))
    via = sorted(inter.get(name, set())) if kind == "element" else []
    sp = special.get((kind, name), [])
    is_item = sorted(item_names.get(name, set())) if kind == "element" else []
    data = "data" in e["classes"] or "empty" in e["classes"] and kind == "attribute"
    is_written = (name in written_attrs) if kind == "attribute" else (name in written)
    out.append(dict(kind=kind, name=name, files=sorted(e["files"]), classes=sorted(e["classes"]),
                    parents=sorted(e["parents"]), columns=carried, via=via, special=sp, item=is_item,
                    written=is_written))

if "--json" in sys.argv:
    json.dump(out, sys.stdout, indent=1)
    sys.exit(0)
d = out


reasons = {
    ("element", "approval-date"): "not mapped: declared at the top level of common-3.0.xsd but referenced by no type in the vendored schemas, so no document carries it",
    ("element", "locale"): "not mapped: the only child of `preferences` (see there)",
    ("attribute", "transient"): "not mapped: always `true` (pattern restriction); it marks `external-id-normalized` and its error as populated by ORCID, not the source. The values themselves are mapped",
}
struct_notes = {
    "activities-summary": "container (record wrapper of the activity collections); items are found below it by suffix",
    "distinctions": "container (affiliation collection); its `affiliation-group`s and items are read",
    "educations": "container (affiliation collection); its `affiliation-group`s and items are read",
    "invited-positions": "container (affiliation collection); its `affiliation-group`s and items are read",
    "memberships": "container (affiliation collection); its `affiliation-group`s and items are read",
    "qualifications": "container (affiliation collection); its `affiliation-group`s and items are read",
    "services": "container (affiliation collection); its `affiliation-group`s and items are read",
    "peer-review-group": "container (peer-review subgroup); `owner` of external ids below it",
    "preferences": "not mapped: researcher preferences (only `locale`); record-3.0.xsd imports the namespace but references no element of it, and there is no section for a standalone document",
    "fuzzy-date": "type-carrier element (common-3.0.xsd); dates are read as `*/year|month|day`",
    "application-orcid": "not mapped: top-level declaration in common-3.0.xsd that no vendored type references",
    "application-group-orcid": "not mapped: top-level declaration in common-3.0.xsd that no vendored type references",
    "group-orcid-identifier": "not mapped: top-level declaration in common-3.0.xsd that no vendored type references",
}

# How each name appears in ORCID's JSON (src/core/json_map.cpp; docs/REFERENCE.md "JSON documents").
json_notes = {
    ("element", "resource-items"): "no member: `resource-item` sits in the research resource",
    ("element", "affiliation-group"): "same; its items in `summaries` wrapper objects",
    ("element", "bulk"): "same; entries are `{\"work\": ...}` / `{\"error\": ...}` wrapper objects",
    ("attribute", "language-code"): "member of the `{\"value\"}` object",
    ("attribute", "currency-code"): "member of the `{\"value\"}` object",
    ("attribute", "transient"): "member (of the `{\"value\"}` object for `external-id-normalized`)",
    ("attribute", "path"): "member `path` (an element in `{uri, path, host}` objects)",
}
DATETIME = {"created-date", "last-modified-date", "read-date", "archived-date", "sent-date", "approval-date",
            "submission-date", "deactivation-date", "actioned-date", "verification-date"}

def json_form(x):
    key = (x["kind"], x["name"])
    if key in json_notes:
        return json_notes[key]
    if x["kind"] == "attribute":
        return "member"
    if x["name"] in DATETIME:
        return "same; `{\"value\": epoch ms}`"
    return "same"

def short(cols, n=4):
    cols = list(cols)
    if len(cols) <= n:
        return ", ".join(f"`{c}`" for c in cols)
    return ", ".join(f"`{c}`" for c in cols[:n]) + f" (+{len(cols) - n})"

def files(fs):
    return ", ".join(f.split("/")[-1].replace(".xsd", "") for f in fs)

rows = []
stats = dict(names=0, elements=0, attributes=0, data=0, data_mapped=0, data_unmapped=0, structural=0, structural_used=0, structural_unused=0, written=0, data_written=0)
unmapped_data = []
for x in d:
    stats["names"] += 1
    stats[x["kind"] + "s"] += 1
    key = (x["kind"], x["name"])
    is_data = x["kind"] == "attribute" or "data" in x["classes"]
    carried = x["columns"] + x["special"]
    if is_data:
        stats["data"] += 1
        if carried:
            stats["data_mapped"] += 1
            status = "mapped"
            where = short(x["columns"]) if x["columns"] else ""
            if x["special"]:
                where = (where + "; " if where else "") + "; ".join(x["special"])
            if "structural" in x["classes"]:
                where += " (also declared as a container elsewhere)"
        else:
            stats["data_unmapped"] += 1
            unmapped_data.append(x)
            status = "not mapped"
            where = reasons.get(key, "not mapped")
    else:
        stats["structural"] += 1
        if x["via"] or x["item"] or x["special"]:
            stats["structural_used"] += 1
            status = "structural"
            bits = []
            if x["item"]:
                bits.append("item element of " + ", ".join(f"`{s}`" for s in x["item"]))
            if x["via"]:
                secs = sorted({c.split(".")[0] for c in x["via"]})
                bits.append("path step in " + ", ".join(f"`{s}`" for s in secs))
            if x["special"]:
                bits.append("; ".join(x["special"]))
            where = "; ".join(bits)
        else:
            note = struct_notes.get(x["name"], "container")
            if note.startswith("not mapped"):
                stats["structural_unused"] += 1
                status = "not mapped"
            else:
                stats["structural_used"] += 1
                status = "structural"
            where = note
    name = ("@" if x["kind"] == "attribute" else "") + x["name"]
    stats["written"] += 1 if x["written"] else 0
    stats["data_written"] += 1 if x["written"] and is_data else 0
    rows.append(f"| `{name}` | {files(x['files'])} | {status} | {where} | {json_form(x)} | {'yes' if x['written'] else ''} |")

print(json.dumps(stats))
print("UNMAPPED_DATA", [x["name"] for x in unmapped_data])
print("\n".join(rows))
