# Similarity audit: yorc-duckdb vs other ORCID and XML software

**Status: not yet measured (TODO).** This file fixes the method so that the
measurement, when it is run, answers a precise question. Until then the
independence claim rests on [`PROVENANCE.md`](PROVENANCE.md): what each
subsystem was written from, and the rule that no code from other ORCID or XML
software was copied, ported or translated.

## Question

Is there evidence that `src/` was copied or adapted from existing software
that reads ORCID XML, or from a general XML parser, rather than written from
the ORCID message schema and the W3C, NIST, ISO and POSIX specifications?

## Comparison corpus (to clone read-only at check time, never committed)

| project | what it is | compare against |
|---|---|---|
| github.com/ORCID/orcid-model | ORCID's Java model of the message schema (JAXB-generated classes for the XSDs we vendor) | `src/core/sections.cpp`: the item and column tables |
| github.com/ORCID/orcid-conversion-lib | ORCID's converter for Public Data File XML, as its README describes | `src/core/sections.cpp`, `src/core/tar.cpp` |
| a general C/C++ XML parser (for example expat or pugixml) | XML 1.0 tokenisers | `src/core/xml.cpp` |
| a C/C++ tar reader (for example libarchive's tar reader) | ustar / pax / GNU readers | `src/core/tar.cpp` |
| a SHA-256 reference implementation | FIPS 180-4 implementations | `src/core/sha256.cpp` |

Record the repository URL and the commit of every clone in this file when
the check is run. Pin the commits, so the numbers can be reproduced.

## Method

The measures are the ones the sibling extension duckdb-marc21 uses in its
`audit/SIMILARITY.md`, so that the two audits read the same way. Each is a
score in [0, 1] per file pair:

| metric | what it measures | what it catches |
|---|---|---|
| `seq_ratio` | `difflib.SequenceMatcher` over whitespace-normalised text | wholesale copying |
| `line_overlap` | fraction of our distinct non-trivial lines (12 or more significant characters) found verbatim in theirs | copied lines, even reordered |
| `tok_jaccard5` | Jaccard similarity of 5-token shingles of the lexed code, with comments and string bodies removed | copied code with changed formatting or comments |
| `ident_jaccard` | Jaccard similarity of identifier vocabularies (length 4 or more, keywords excluded) | copied naming with rewritten logic |
| `comment_sh3` | Jaccard similarity of 3-word shingles of the comment prose | copied comments and documentation |

Steps:
1. **Calibrate.** Measure two unrelated files of the same language (the
   floor), a file against a lightly edited copy of itself (the ceiling), and
   our `xml.cpp` against a second, independent XML tokeniser.
2. **Measure** each row of the corpus table.
3. **Expect a vocabulary floor.** Identifiers and string literals forced by
   the schema (`work-summary`, `put-code`, `external-id-type`, ...) and by
   the specifications (SHA-256 round constants, the ustar field offsets) are
   shared by any correct implementation. Report them separately, as the
   expected floor, rather than as evidence either way.
4. **Report** per file: the five scores, the calibration anchors, and a
   one-paragraph reading.

Cross-language pairs (our C++ against Java) make `seq_ratio`,
`line_overlap` and `tok_jaccard5` close to meaningless. For those, report
`ident_jaccard` and `comment_sh3` only, plus a structural comparison: the
item and column tables against the class and field structure of the Java
model.

## Tooling

`audit/tools/similarity_check.py` does not exist yet. It is to be written
with the Python 3 standard library only, and to take
`--repo /path/to/yorc-duckdb --clones /path/to/clones` the way the marc21
tool does. Writing and running it is a maintainer step. It needs network
access for the clones, and the clones are not committed.
