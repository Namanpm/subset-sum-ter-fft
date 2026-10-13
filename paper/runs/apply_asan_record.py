"""Apply a finished recorded ASan differential run to the paper (DOCX only).

    python paper/runs/apply_asan_record.py [results/arwlf/asan-differential-<date>]

Fails closed: unless the record shows exit code 0, no sanitizer report and the
line "341 checks, 0 failures", nothing is copied or edited.  On success it
freezes record.json and output.log into paper/data (listed with SHA-256 in
input_manifest.json), replaces the "historical development log" ASan wording
in manuscript.md and Table 7, rebuilds tables/figures and the DOCX (TeX and PDF
preserved), and refreshes the copy in "Research papers/".  Running it twice is
harmless: already-applied text is detected and left alone.
"""
import hashlib, json, shutil, subprocess, sys
from pathlib import Path

PAPER = Path(__file__).resolve().parents[1]
REPO = PAPER.parent
DATA = PAPER / "data"
EXPECTED = "341 checks, 0 failures"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fail(message: str) -> None:
    sys.exit(f"NOT APPLIED: {message}")


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text and old not in text:
        return  # already applied
    if text.count(old) != 1:
        fail(f"expected exactly one occurrence of the old wording in {path.name}; edit it by hand")
    path.write_text(text.replace(old, new), encoding="utf-8")


def main() -> None:
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    if len(sys.argv) == 2:
        run = Path(sys.argv[1])
    else:
        runs = sorted((REPO / "results" / "arwlf").glob("asan-differential-*/record.json"))
        if not runs:
            fail("no results/arwlf/asan-differential-*/record.json; the run has not finished")
        run = runs[-1].parent
    record_path, log_path = run / "record.json", run / "output.log"
    if not record_path.is_file() or not log_path.is_file():
        fail(f"{run} lacks record.json or output.log")
    record = json.loads(record_path.read_text(encoding="utf-8-sig"))
    log = log_path.read_text(encoding="utf-8-sig", errors="replace")
    if record.get("exit_code") != 0:
        fail(f"exit code {record.get('exit_code')}: the test failed or was interrupted; report it, do not apply")
    if record.get("sanitizer_report") is not False or "AddressSanitizer" in log:
        fail("AddressSanitizer reported an error; read output.log")
    if EXPECTED not in str(record.get("summary_line", "")) or EXPECTED not in log:
        fail(f"summary line is {record.get('summary_line')!r}, expected {EXPECTED!r}")

    date = record["started_utc"][:10]
    hours = record["elapsed_s"] / 3600
    shutil.copyfile(record_path, DATA / "asan_differential_record.json")
    shutil.copyfile(log_path, DATA / "asan_differential_output.log")
    manifest_path = DATA / "input_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    rel = run.resolve().relative_to(REPO).as_posix()
    entries = {"asan_differential_record.json": f"{rel}/record.json (recorded ASan differential run)",
               "asan_differential_output.log": f"{rel}/output.log (complete test output)"}
    manifest["inputs"] = [r for r in manifest["inputs"] if r["file"] not in entries]
    manifest["inputs"] += [{"file": f, "origin": o, "sha256": sha256(DATA / f)} for f, o in entries.items()]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    replace_once(PAPER / "manuscript.md",
                 "A separate direct differential log records all 341 checks with zero failures and no sanitizer diagnostic. "
                 "These are historical development logs; the direct log does not retain a timestamped command and exit-code wrapper.",
                 f"A separately recorded rerun of the differential test under AddressSanitizer ({date}, {hours:.1f} hours) completed "
                 "all 341 checks with zero failures, exit code 0 and no sanitizer diagnostic; its command, executable SHA-256, "
                 "timestamps and complete output are retained with the supplement.")
    replace_once(PAPER / "build_results.py",
                 "57 passed; differential timed out at 1 h. Separate direct log: 341 checks, 0 failures; no sanitizer diagnostic recorded",
                 f"57 passed; differential timed out at 1 h. Recorded rerun of the differential test ({date}): 341 checks, 0 failures, exit 0, no sanitizer diagnostic")
    replace_once(PAPER / "build_results.py",
                 "Test evidence comes from saved development logs, not a fresh run; the direct ASan log lacks timestamped command/exit-code metadata.",
                 "Release test evidence comes from a saved development log; the differential ASan result is a recorded rerun (data/asan_differential_record.json).")

    python = sys.executable
    subprocess.run([python, "build_results.py"], cwd=PAPER, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([python, "build_article.py", "--docx-only"], cwd=PAPER, check=True)
    copy = REPO / "Research papers" / "ARWLF_Naman_PM.docx"
    if copy.parent.is_dir():
        shutil.copyfile(PAPER / "ARWLF_Naman_PM.docx", copy)
    print(f"APPLIED: ASan rerun of {date} ({hours:.1f} h) is in Section 8.7 and Table 7; DOCX rebuilt.")


if __name__ == "__main__":
    main()
