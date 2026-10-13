WITHDRAWN RUN - DO NOT USE AS EVIDENCE

complete-merge-WITHDRAWN-degenerate-lambda.csv is the output of the first
execution of ter_tiled_complete_merge_bench. It is retained for transparency
and is excluded from every analysis artifact.

Why it was withdrawn, before any of its numbers were reported:

1. The benchmark held b2=8, b1=18 fixed for every list size, so Q = 2^10 at
   every point while L grew to 1024. That makes lambda = L^2/Q reach ~1024,
   against the 2-4 of the declared TER paper scaling.
2. At Q = 2^10 the tile policy gives tile_bits = min(15, 10) = 10, hence
   S = Q, P = 1 bucket and 1 window. The tiled path degenerates to a single
   window, so the comparison could not measure tiling at all.
3. Its memory columns were not comparable. src/merge.cpp assigns the tiled
   path working_set_budget_bytes = fixed_bytes + candidate_bytes, where
   candidate_bytes is a derived ceiling consuming the whole remaining budget,
   not a measured peak. The baseline reports an actual pair_build_peak. The
   two fields do not denote the same quantity.
4. The L=2048 point returned ResourceLimit as a consequence of (1).

The instrument was repaired and rerun. No number from this file appears in
analysis/, figures/, paper-facts.json or the final report.
