# ORCID Public Data File index

`index.csv` lists every file of every ORCID Public Data File release that ORCID
publishes on figshare (https://orcid.figshare.com/), one row per file:

| column | meaning |
|---|---|
| `release` | figshare item title, e.g. "ORCID Public Data File 2025" |
| `published` | publication date of the release |
| `doi` | DOI of the release |
| `landing_url` | human-readable release page |
| `file_name` | e.g. `ORCID_2025_10_summaries.tar.gz` |
| `kind` | `summaries`, `activities` or `other` |
| `size_bytes`, `md5` | as reported by figshare |
| `download_url` | direct URL; works with httpfs and HTTP range requests |
| `license` | as declared by ORCID (CC0) |

It is generated, not hand-edited: run `scripts/orcid_dumps_index.sh` (from any
directory; needs `curl`, `jq` and access to api.figshare.com), review the diff,
and commit. The committed copy lists the releases published when it was last run.
