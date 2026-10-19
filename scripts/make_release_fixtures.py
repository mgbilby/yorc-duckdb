"""Build two synthetic ORCID Public Data File "releases" for the orcid_changes
demo (test/data/releases/, test/sql/orcid_docs_examples.test, README).

    python3 -I scripts/make_release_fixtures.py SUMMARIES_DIR OUT_DIR

SUMMARIES_DIR holds summaries from the ORCID Public Data File 2025 (CC0), as
<iD>.xml (for example an extracted sample of the summaries archive). Both archives use the dump's layout,
<release>/<last three characters of the iD>/<iD>.xml, and are byte-for-byte
reproducible (sorted members, fixed times and owners, gzip without a name or
time stamp).

Release "2024" holds the five records below unchanged. Release "2025" applies
deterministic edits, standing in for a year of changes:
  0009-0008-6715-4000  a work's title revised
  0000-0002-9600-9004  the first work group removed (the work is gone)
  0009-0004-7374-2002  family name and employment role title changed
  0009-0000-8621-6002  unchanged
  0000-0002-2759-4000  not in 2025 (record removed from the release)
  0000-0001-5739-7000  new in 2025
"""
import gzip
import io
import re
import sys
import tarfile
from pathlib import Path

SRC, OUT = Path(sys.argv[1]), Path(sys.argv[2])
OLD_IDS = ["0009-0008-6715-4000", "0000-0002-9600-9004", "0009-0004-7374-2002",
           "0009-0000-8621-6002", "0000-0002-2759-4000"]
NEW_ONLY = ["0000-0001-5739-7000"]


def edit(orcid, xml):
    if orcid == "0009-0008-6715-4000":
        old = "<common:title>Alternative Outcomes, Surprise, and Predicting Replication of PsychologicalResearch</common:title>"
        assert old in xml
        return xml.replace(old, "<common:title>Alternative Outcomes, Surprise, and Predicting Replication of Psychological Research</common:title>")
    if orcid == "0000-0002-9600-9004":
        groups = list(re.finditer(r"<activities:group>.*?</activities:group>\s*", xml, re.S))
        g = next(m for m in groups if "<work:work-summary" in m.group(0))
        return xml[:g.start()] + xml[g.end():]
    if orcid == "0009-0004-7374-2002":
        for old, new in [("<personal-details:family-name>Nademi</personal-details:family-name>",
                          "<personal-details:family-name>Nademi-Rahimi</personal-details:family-name>"),
                         ("<common:role-title>PhD Researchers </common:role-title>",
                          "<common:role-title>Postdoctoral Researcher</common:role-title>")]:
            assert old in xml
            xml = xml.replace(old, new)
        return xml
    return xml


def write(release, docs):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for orcid in sorted(docs):
            data = docs[orcid].encode("utf-8")
            info = tarfile.TarInfo(f"{release}/{orcid[-3:]}/{orcid}.xml")
            info.size, info.mtime, info.mode, info.uid, info.gid = len(data), 0, 0o644, 0, 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(data))
    with open(OUT / f"{release}.tar.gz", "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0, compresslevel=9) as gz:
            gz.write(buf.getvalue())


OUT.mkdir(parents=True, exist_ok=True)
read = {o: (SRC / f"{o}.xml").read_text(encoding="utf-8") for o in OLD_IDS + NEW_ONLY}
write("ORCID_2024_10_summaries", {o: read[o] for o in OLD_IDS})
new = {o: edit(o, read[o]) for o in OLD_IDS if o != "0000-0002-2759-4000"}
new.update({o: read[o] for o in NEW_ONLY})
write("ORCID_2025_10_summaries", new)
