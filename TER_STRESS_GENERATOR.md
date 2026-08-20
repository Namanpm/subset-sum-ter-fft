# TER adversarial merge-stress generator

`ter::build_stress_snapshot()` is a separate fixture generator for pathological
`L2 -> L1` merge workloads. It is not a paper-rounded generator, a FullTer run,
or evidence about representative random density-one instances. Its output is
labelled `ADVERSARIAL_STRESS` by `ter_stress_bench` for that reason.

## Contract

The stress builder starts with `build_snapshot(SnapshotConfig)`. It then makes
deterministic, quotient-aware selections from three independently valid TER
child pools. It never invents a quotient residue, target, digit vector, or
sidecar: `audit_snapshot_children()` recomputes every residue from the original
vectors and instance values before the fixture is returned.

The input API is in `include/ter/stress_generator.hpp`:

```cpp
ter::TerStressConfig config;
config.source = normal_snapshot_config;
config.candidate_pool_size = 256;
config.requested_list_sizes = {32, 64, 96};
config.prefilter_bits = 2;
config.bucket_profile = ter::BucketProfile::HotBuckets;
config.hot_bucket_count = 1;
config.target_hot_mass_percent = 60;
config.hot_mass_tolerance_percent = 15;
auto result = ter::build_stress_snapshot(config);
```

Failure is explicit. Invalid combinations return `InvalidConfiguration`; a
pool that cannot satisfy a required list size, witness lower bound, or measured
hot-bucket tolerance returns `GenerationIncomplete`; infeasible memory returns
`ResourceLimit`. The builder never silently reduces a requested list size or
relaxes a skew target.

`StressMode` is also validated rather than being a decorative label:
`BucketSkew` requires equal A/B/C sizes, `GroupSizeSkew` requires unequal sizes
with the uniform control profile, `SolutionMultiplicity` requires a planted or
certified-witness mode, `ArithmeticStructure` requires an actual hot-residue
profile, and `QuotientBoundary` requires the requested live width to equal
`b1-b2`. `Composite` deliberately permits compatible combinations.

## Stress axes

### Bucket / quotient-residue skew

`BucketProfile::Uniform` round-robins actual `entry.q mod 2^prefilter_bits`
buckets. `HotBuckets` selects the configured fraction of entries from bucket
IDs `[0, hot_bucket_count)`, measures the result, and rejects it if the measured
mass misses `target_hot_mass_percent +/- hot_mass_tolerance_percent`.

Telemetry reports total/nonempty buckets, mean and maximum occupancy, variance,
collision count, hot mass, and normalized Shannon entropy. Those values are
computed from cached quotient residues *after* the full child audit has
independently recomputed them from the original arithmetic.

### A/B/C list imbalance

`requested_list_sizes` controls each of the three selected lists separately.
For example `{32, 64, 96}` is a 1:2:3 fixture; `{5'000, 50'000, 120'000}` is
valid only if its candidate pool and declared memory budget can actually support
it. Selection preserves the source lists' canonical order and reassigns entry
IDs, so the normal merge validators continue to apply.

### Controlled multiplicity

`NoPlantedSolution` makes no witness claim. It does not claim that the total
number of solutions is zero.

`SinglePlantedSolution` retains and independently validates the normal
generator's one planted witness. It also does not claim exact uniqueness.

`MultiplePlantedSolutions` and `DenseSolutionStress` run the bounded independent
cubic oracle on the candidate pool, choose `required_distinct_witnesses` exact
triples deliberately into the output lists, then audit each again after ID
remapping. The telemetry distinguishes deliberately retained/guaranteed
witnesses from the exact local count established by the cubic oracle. If the
oracle cannot run under its existing cap, this is a resource refusal, not a
statement about the mathematical solution count.

### Width boundaries

The ordinary merge representation uses `uint32_t` quotient residues, so normal
arithmetic accepts 31 and 32 quotient bits but rejects 33. The stress API makes
that boundary explicit with `describe_quotient_boundary()`. It does not fake a
33-bit merge by truncating it to 32 bits. A widened exact merge representation
would be a separate solver/algorithm change and is intentionally out of scope.

## Determinism and memory

The same complete `TerStressConfig`, including `source.seed`, produces the same
source snapshot, selections, telemetry, and certified provenance. Timings are
not expected to replay.

Before source generation, the builder reserves the selected fixture and
selection workspace, passes only the remaining declared memory budget to the
normal source generator, and releases the full source pool before final local
oracle work. Resource arithmetic uses `ter/limitations.hpp` checked helpers.

## Examples

1. Normal-ish control: `bucket_profile = Uniform`, equal list sizes, and
   `SinglePlantedSolution`.
2. High skew: `HotBuckets`, one hot bucket, 60--90 percent configured mass,
   with a tolerance appropriate to the available candidate pool.
3. Imbalance: set `{1'000, 10'000, 25'000}` only with a candidate pool and
   memory budget large enough for the largest list.
4. Boundary: use normal source configurations `(b1,b2) = (32,1)` and `(33,1)`
   for live 31/32-bit fixtures; call `describe_quotient_boundary(33)` to record
   the correct native rejection.
5. n=72-style geometry: use the same API with a 72-dimensional valid source
   shape and sizes supplied as a benchmark configuration. This is adversarial
   merge geometry, not a claim that the fixture is an authentic complete n=72
   FullTer execution. The API deliberately does not hard-code historical list
   measurements as universal TER constants.

## Benchmark row

`ter_stress_bench` emits one CSV header and one `ADVERSARIAL_STRESS` row. It
includes seed, dimensions, arithmetic widths, list sizes, bucket geometry,
distribution metrics, candidate tile settings, auxiliary memory, phase timing,
and actual survivor count. `prediction_available=0` is intentional: no planner
is implemented yet, so `predicted_survivor_count=NA` rather than fabricated
model output. `verification_ns=NA` under the benchmark policy because that
existing policy does not separately time validation; zero would be a lie.
