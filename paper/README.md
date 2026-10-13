# Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting

Author: Naman Menasagi (Independent Researcher, Bengaluru, India). Intended journal: ACM Transactions on Algorithms (TALG).

The current article is **ARWLF_Naman_PM.docx**, last built 14 October 2026 on ACM's Word submission template (`template/acm_submission_template.docx`). It contains 214 editable native Word equations, five figures, fourteen tables, sixteen cited references and a 223-word abstract (counts from `build_article.py` and `validate_docx_only.py`). On 14 October every page was exported to PDF with Word (31 pages) and LibreOffice 26.2 (32 pages) and reviewed. The 13 October build on the earlier theory-paper template is kept unchanged in `archive/2026-10-13-theory-template/`.

The article uses only the ACM template's own styles. Section and subsection headings number themselves, and the build fails unless the manuscript's section numbers equal that sequence, because the text cites sections by number; the reference list numbers itself in citation order. The front matter follows the template: short title, author and affiliation, abstract, CCS concepts, additional keywords and an ACM reference block. The author name and affiliation line come from `AUTHOR` and `AFFILIATION` in `build_article.py`; edit them there, not in Word, because saving in Word rewrites the equations (for example the minus sign becomes a hyphen and ℓ becomes l). The CCS 2012 concept, chosen by the author, is Theory of computation → Data structures design and analysis. The text font is Linux Libertine O and Linux Biolinum O; install their OpenType files before rendering. Word cannot embed these OpenType-CFF fonts in a PDF, so its PDF export draws the text as 600-dpi images over an invisible, searchable text layer; LibreOffice embeds them as fonts.

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

The builder clones `template/acm_submission_template.docx` (its SHA-256 is checked by the builder) and keeps its section geometry and eleven package parts byte-identical, including styles, numbering, footers and settings. It writes only the document body, relationships, content types, core properties and the footnotes part (without the template's sample author note), and drops the template's six sample images. `validate_docx_only.py` checks that every body paragraph uses a template style and that the template's styles, numbering, footers and settings are unchanged; Office schema validation also passes against the template. The 9 October continuation changed no research code. Evidence added on 11-12 October (the hash-join baseline, the design-B comparison and the ASan differential record) is listed in `data/README.md`; `build_results.py` hash-checks it and checks the hash-join and design-B contents. The 13 October revision fixed the equation rendering, tightened the abstract and Appendix C, and added the build-source recovery check (`../reviewer/check_build_sources.py`) cited in Data and code availability; it changed no research code and no recorded result. The 14 October revision moved the article onto ACM's template. Apart from the template's added front matter, it changed no wording, number, equation, table entry or figure; presentation now follows the template, for example "Table 1:" captions, capitalized section headings, an ALGORITHM caption box and automatically numbered references.

For what every file in the repository is, and which older results must not be cited, see `../LEGACY_MAP.md`. See `SUBMISSION_NOTES.md` for author metadata and reviewer artifact access needed before journal upload. The article is a research narrative with formal results and experiments; review notes are separate QA companions.
