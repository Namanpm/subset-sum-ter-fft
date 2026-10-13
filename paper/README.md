# Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting

Author: Naman PM. Intended journal: ACM Transactions on Algorithms (TALG).

The current article is **ARWLF_Naman_PM.docx**, last built 13 October 2026 in the selected Theory-Focused Research Paper template. It contains 214 editable native Word equations, five figures, fourteen tables, sixteen cited references and a 223-word abstract (counts from `build_article.py` and `validate_docx_only.py`). On 13 October all 33 pages were exported to PDF with both Word and LibreOffice 26.2 and reviewed page by page.

Display equations are Word display-mode math (`m:oMathPara`) with their number inside the equation. The manuscript avoids floor and ceiling brackets and the divides bar, because LibreOffice's Word-math import draws those glyphs as slashes; it also avoids the word div, which LibreOffice turns into ÷. `validate_docx_only.py` fails if a floor or ceiling delimiter object, a literal backtick, or a display equation outside display mode reaches the DOCX.

The DOCX includes the complete dimension-108 prepared-root comparison (55.2–57.4× across ARWLF/dispatcher passes), the independently verified dimension-72 staged solve and the occupancy/scaling section. Dimension-144 values are hypothetical wider-key cost scenarios: the frozen backend cannot execute that dimension. Frontier calculations assume unchanged observed Intel cost on every hypothetical AMD CPU core, perfect scaling and no GPU execution. They are not measured Frontier runtimes.

`manuscript.md` is the current content source. `working/scaling_section.md` extracts Section 9. `scaling_model.py` and its frozen inputs reproduce the scenarios. Bibliography metadata are in `references.json` and `references.bib`; only the sixteen cited sources appear in the article.

All pre-existing PDF and TeX files and `ARWLF_submission_sources.zip` are retained unchanged at the user's request. They describe the historical October-4 article and do not match the current DOCX. The earlier release manifest, hashes and PDF checks apply to that historical package. Current verification is `validate_docx_only.py`, which also checks that every protected PDF/TeX file is unchanged.

## Reproduce the current DOCX

From `paper/`, with Python, python-docx, lxml, NumPy, Matplotlib and the Microsoft Office math stylesheet available:

```powershell
python scaling_model.py
python build_results.py --tables-only
python build_article.py --docx-only
python validate_docx_only.py --portable
```

Normal model operation reads frozen JSON only. The tables-only flag reconciles raw input hashes, medians, ratios and cluster intervals without writing figure assets. The DOCX-only flag preserves TeX. Portable validation checks source-expression preservation, every table row, model probabilities and the DOCX package using distributable files. Its report is written under ignored `qa/portable/`; unavailable preservation and rendered-page checks are null, never passes.

For strict local preservation, keep every file listed in `qa/continuation-20261009/protected_files_before.json`, run `./render_docx_pages.ps1`, then run `python validate_docx_only.py` without flags. The default mode requires that inventory, every file it lists, and `qa/continuation-20261009/pages/pages.json`. The inventory is the only `qa/` file in git; the page renders and the third-party PDFs under `Research papers/` are intentionally absent from a fresh clone, so the default mode runs only on the author's machine. The renderer opens Word read-only and does not save the document or create PDFs.

Completed experiment scripts now live in `runs/`, formerly `pending_runs/`. Recorded historical commands keep their original paths.

The builder clones `template/reference.docx` (SHA-256 recorded in `working/artifact.md`) and preserves its section geometry and twelve untouched package parts. Office schema validation also passes against that reference. The 9 October continuation changed no research code. Evidence added on 11-12 October (the hash-join baseline, the design-B comparison and the ASan differential record) is listed in `data/README.md`; `build_results.py` hash-checks it and checks the hash-join and design-B contents. The 13 October revision fixed the equation rendering, tightened the abstract and Appendix C, and added the build-source recovery check (`../reviewer/check_build_sources.py`) cited in Data and code availability; it changed no research code and no recorded result.

For what every file in the repository is, and which older results must not be cited, see `../LEGACY_MAP.md`. See `SUBMISSION_NOTES.md` for author metadata and reviewer artifact access needed before journal upload. The article is a research narrative with formal results and experiments; review notes are separate QA companions.
