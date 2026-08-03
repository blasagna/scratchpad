//! Compares `matrix_ops`' naive implementation against faer and nalgebra on
//! the four operations the CLI supports.
//!
//! The counterpart of `matrix_ops/bench/compare.cpp`, which does the same for
//! the C++ port against Eigen and xtensor. Same sizes, same operations, so the
//! two tables in `matrix_ops/README.md` describe the same work.
//!
//! Run through `run.sh`, which builds optimized and runs the agreement test
//! first. `cargo bench` on its own is a valid but ungated way in.

use std::time::Duration;

use criterion::measurement::WallTime;
use criterion::{
    BenchmarkGroup, BenchmarkId, Criterion, SamplingMode, criterion_group, criterion_main,
};
use faer::{Par, Scale};
use matrix_ops_bench::{Fixture, SCALAR, SIZES, check};

/// Threads faer may use. nalgebra is single-threaded by construction — its f64
/// GEMM goes through `matrixmultiply` without the `threading` feature — and
/// ours is a plain loop, so faer is the only one with anything to configure.
///
/// This is what makes the Rust benchmark simpler than the C++ one, where Eigen
/// and OpenBLAS each need the other quiesced and neither can be measured fairly
/// in a run configured for the other.
fn set_parallelism() -> usize {
    let threads = std::env::var("MATRIX_OPS_BENCH_THREADS")
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|n| *n > 0)
        .unwrap_or_else(|| {
            std::thread::available_parallelism()
                .map(|n| n.get())
                .unwrap_or(1)
        });

    faer::set_global_parallelism(if threads == 1 {
        Par::Seq
    } else {
        Par::rayon(threads)
    });
    threads
}

/// Rough cost of one iteration, used only to choose criterion's budget.
///
/// Per-iteration cost spans five orders of magnitude across this matrix of
/// benchmarks — a 64x64 scale is microseconds, the naive 1024x1024 multiply is
/// about three seconds — and one global configuration cannot serve both. Being
/// wrong here costs time or earns a criterion warning; it does not affect the
/// reported numbers.
fn estimate(op: &str, naive: bool, n: usize) -> Duration {
    let elements = (n * n) as u32;
    match (op, naive) {
        // A blocked GEMM, at roughly 0.02 ns per multiply-add.
        ("mul", false) => Duration::from_nanos(1).mul_f64(0.02) * elements * n as u32,
        // The naive triple loop, about 3 ns per multiply-add.
        ("mul", true) => Duration::from_nanos(3) * elements * n as u32,
        // Everything else is one pass over the elements.
        _ => Duration::from_nanos(1) * elements,
    }
}

/// Gives the group a budget suited to how slow the next benchmark is.
///
/// The important part is the sampling mode. criterion's default linear sampling
/// runs 1, 2, 3, ... N iterations per sample, about 5000 iterations at the
/// default sample size — fine at microseconds, absurd at seconds. Flat sampling
/// runs a fixed count per sample, so the floor becomes `sample_size * per_iter`
/// and nothing else.
fn budget(group: &mut BenchmarkGroup<'_, WallTime>, per_iter: Duration) {
    if per_iter < Duration::from_millis(1) {
        // criterion's defaults are already right for a fast benchmark.
        return;
    }
    group.sampling_mode(SamplingMode::Flat);
    group.sample_size(10); // criterion's minimum
    group.warm_up_time(Duration::from_millis(200));
    group.measurement_time((per_iter * 12).max(Duration::from_secs(1)));
}

fn compare(c: &mut Criterion) {
    let threads = set_parallelism();
    eprintln!("faer parallelism: {threads} thread(s); nalgebra is single-threaded");

    let fixtures: Vec<Fixture> = SIZES.iter().map(|n| Fixture::new(*n)).collect();

    // The gate, before any timing. A disagreement means the three
    // implementations are not computing the same thing, and no timing below
    // would mean anything.
    for fixture in &fixtures {
        if let Err(disagreement) = check(fixture) {
            panic!("at {0}x{0}: {disagreement}", fixture.n);
        }
    }

    for op in ["add", "sub", "mul", "scale"] {
        let mut group = c.benchmark_group(op);
        for fixture in &fixtures {
            let n = fixture.n;
            let (a, b) = &fixture.ours;
            let (fa, fb) = &fixture.faer;
            let (na, nb) = &fixture.nalgebra;

            budget(&mut group, estimate(op, true, n));
            group.bench_with_input(BenchmarkId::new("ours", n), &n, |bench, _| match op {
                "add" => bench.iter(|| a.add(b).unwrap()),
                "sub" => bench.iter(|| a.sub(b).unwrap()),
                "mul" => bench.iter(|| a.mul(b).unwrap()),
                _ => bench.iter(|| a.scale(SCALAR)),
            });

            budget(&mut group, estimate(op, false, n));
            group.bench_with_input(BenchmarkId::new("faer", n), &n, |bench, _| match op {
                "add" => bench.iter(|| fa + fb),
                "sub" => bench.iter(|| fa - fb),
                "mul" => bench.iter(|| fa * fb),
                _ => bench.iter(|| fa * Scale(SCALAR)),
            });

            group.bench_with_input(BenchmarkId::new("nalgebra", n), &n, |bench, _| match op {
                "add" => bench.iter(|| na + nb),
                "sub" => bench.iter(|| na - nb),
                "mul" => bench.iter(|| na * nb),
                _ => bench.iter(|| na * SCALAR),
            });
        }
        group.finish();
    }
}

criterion_group!(benches, compare);
criterion_main!(benches);
