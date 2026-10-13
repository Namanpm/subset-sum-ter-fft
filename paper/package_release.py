"""Package the reviewed paper and verify a standalone source rebuild.

Writes only below this paper directory. Native benchmarks and research code
are never invoked. Full-text copies of third-party research papers are excluded.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent
ARCHIVE = ROOT / 'ARWLF_submission_sources.zip'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

names = [
    'ARWLF_Naman_PM.pdf', 'ARWLF_Naman_PM.docx', 'manuscript.md',
    'manuscript.tex', 'references.bib', 'references.json', 'README.md',
    'SUBMISSION_NOTES.md', 'claim_ledger.md', 'build_results.py',
    'build_article.py', 'math_native.py', 'validate_article.py',
    'compile.ps1', 'render_word.ps1', 'package_release.py',
    'working/artifact.md', 'working/literature_inventory.json',
    'working/math_inventory.json',
    'qa/article_validation.json', 'qa/build_manifest.json',
    'qa/results_build_validation.json', 'qa/pdf_visual_review.md',
    'qa/docx_visual_review.md', 'qa/numerical_render_review.md',
    'qa/latex-pass-3.txt', 'qa/word-render.pdf',
]
for folder in ('figures', 'tables', 'data', 'template'):
    names.extend(p.relative_to(ROOT).as_posix() for p in (ROOT / folder).iterdir() if p.is_file())
names = sorted(set(names))
for name in names:
    if not (ROOT / name).is_file():
        raise FileNotFoundError(name)

validation = json.loads((ROOT / 'qa/article_validation.json').read_text(encoding='utf-8'))
assert digest(ROOT / 'ARWLF_Naman_PM.pdf') == validation['ARWLF_Naman_PM.pdf']['sha256']
assert digest(ROOT / 'qa/word-render.pdf') == validation['word-render.pdf']['sha256']
assert validation['ARWLF_Naman_PM.pdf']['pages'] == 23
assert validation['word-render.pdf']['pages'] == 23
assert validation['native_word_equations'] == 157
for review in ('pdf_visual_review.md', 'docx_visual_review.md', 'numerical_render_review.md'):
    assert 'PASS' in (ROOT / 'qa' / review).read_text(encoding='utf-8')

manifest = {
    'title': 'Adaptive Wide-Leaf Filtering for Exact Ternary Representation Root Reporting',
    'author': 'Naman PM', 'intended_venue': 'ACM Journal of Experimental Algorithmics',
    'prepared': '2026-10-04', 'pdf_pages': 23, 'docx_render_pages': 23,
    'native_word_equations': 157, 'figures': 5, 'tables': 8, 'cited_references': 14,
    'research_code_modified': False, 'native_benchmarks_run': False,
    'files': {name: {'bytes': (ROOT / name).stat().st_size, 'sha256': digest(ROOT / name)} for name in names},
    'exclusions': ['large native snapshots', 'native executables', 'third-party full-text papers',
                   'Claude session transcript', 'superseded renders', 'per-page review images'],
}
(ROOT / 'release_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
(ROOT / 'SHA256SUMS.txt').write_text(''.join(f"{item['sha256']}  {name}\n" for name, item in manifest['files'].items()), encoding='utf-8')
names += ['release_manifest.json', 'SHA256SUMS.txt']
with zipfile.ZipFile(ARCHIVE, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
    for name in sorted(names):
        info = zipfile.ZipInfo(name, date_time=(2026, 10, 4, 12, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        bundle.writestr(info, (ROOT / name).read_bytes())

check = ROOT / 'qa/source_bundle_check'
check.mkdir(parents=True, exist_ok=True)
assert check.resolve().is_relative_to(ROOT)
with zipfile.ZipFile(ARCHIVE) as bundle:
    assert bundle.testzip() is None
    assert set(bundle.namelist()) == set(names)
    for name in names:
        assert not Path(name).is_absolute() and '..' not in Path(name).parts
        assert hashlib.sha256(bundle.read(name)).hexdigest() == digest(ROOT / name)
    bundle.extractall(check)

results = {}
for script in ('build_results.py', 'build_article.py'):
    run = subprocess.run([sys.executable, str(check / script)], cwd=check,
                         capture_output=True, text=True, encoding='utf-8')
    (check / 'qa' / (script + '.output.txt')).write_text(run.stdout + run.stderr, encoding='utf-8')
    if run.returncode:
        raise RuntimeError(f'{script}: {run.stdout}\n{run.stderr}')
    results[script] = 'passed in extracted standalone bundle'

derived = ['tables/tables.json', 'figures/figures.json', 'data/recompute_report.json', 'manuscript.tex']
derived += [f'figures/{name}.{suffix}' for name in ('topology', 'pipeline', 'dense', 'crossover', 'target') for suffix in ('png', 'pdf')]
for name in derived:
    assert digest(check / name) == digest(ROOT / name), name
with zipfile.ZipFile(ROOT / 'ARWLF_Naman_PM.docx') as original, zipfile.ZipFile(check / 'ARWLF_Naman_PM.docx') as rebuilt:
    assert set(original.namelist()) == set(rebuilt.namelist())
    for name in original.namelist():
        assert original.read(name) == rebuilt.read(name), name

report = {
    'archive': ARCHIVE.name, 'sha256': digest(ARCHIVE), 'bytes': ARCHIVE.stat().st_size,
    'entries': len(names), 'zip_crc': 'passed', 'all_archived_hashes': 'passed',
    'standalone_rebuild': results, 'derived_hashes': {name: digest(ROOT / name) for name in derived},
    'docx_rebuild': 'all package parts byte-identical; ZIP timestamps may differ',
}
(ROOT / 'qa/package_validation.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, indent=2))
