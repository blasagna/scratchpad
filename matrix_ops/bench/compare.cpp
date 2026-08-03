// Compares matrix_ops' hand-written implementation against Eigen and xtensor
// on the four operations the CLI supports.
//
// Two things are measured, and the correctness check gates the timings: a
// benchmark that silently compares three different answers is worse than no
// benchmark. Every operation is verified elementwise against our result before
// any of them are timed.
//
// Run through bench/run.sh, which builds with --config=opt. The default Bazel
// build is fastbuild (-O0), and timings taken there say nothing.

#include <Eigen/Dense>
#include <xtensor-blas/xlinalg.hpp>
#include <xtensor/containers/xarray.hpp>

// <array> and <algorithm> are for kSizes, std::ranges::copy, and std::max;
// they arrive transitively through Eigen and xtensor, but naming them keeps a
// dependency bump from breaking this target for no visible reason.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "matrix_ops/cpp/matrix.hpp"

namespace {

using matrix_ops::Matrix;

// Square sizes benchmarked. 64 fits in L1 and shows call overhead; 1024 is
// large enough that GEMM's cache blocking is the whole story.
constexpr std::array<std::size_t, 3> kSizes{64, 256, 1024};

// Repetitions per measurement. The best is reported rather than the mean: the
// fastest run is the one least perturbed by unrelated system noise.
constexpr int kRepetitions = 5;

// Tolerance for the cross-library agreement check. The three implementations
// accumulate in different orders, so bitwise equality is not expected; this is
// relative to the magnitude of the entries.
constexpr double kTolerance = 1e-9;

// Eigen defaults to column-major, but our Matrix and xt::xarray are both
// row-major. Pinning Eigen to row-major too is what makes .data() directly
// comparable across all three -- and it is the fairer benchmark, since a
// column-major Eigen would be measured on a different memory layout than its
// competitors. Getting this wrong is not subtle in the results: element 0
// still agrees and element 1 does not.
using RowMajorXd =
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

std::vector<double> random_values(std::size_t count, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);
  std::vector<double> values(count);
  std::ranges::generate(values, [&] { return dist(rng); });
  return values;
}

Matrix make_matrix(std::size_t n, const std::vector<double> &values) {
  std::optional<Matrix> m = Matrix::create(n, n);
  if (!m) {
    std::fprintf(stderr, "failed to allocate a %zux%zu matrix\n", n, n);
    std::exit(1);
  }
  std::ranges::copy(values, m->values().begin());
  return *std::move(m);
}

RowMajorXd make_eigen(std::size_t n, const std::vector<double> &values) {
  const Eigen::Index dim = static_cast<Eigen::Index>(n);
  return RowMajorXd(Eigen::Map<const RowMajorXd>(values.data(), dim, dim));
}

xt::xarray<double> make_xtensor(std::size_t n,
                                const std::vector<double> &values) {
  xt::xarray<double> a = xt::zeros<double>({n, n});
  std::ranges::copy(values, a.begin());
  return a;
}

// OpenBLAS's runtime thread-count control. Declared by hand because there is no
// BLAS header on this machine -- see //third_party/xtensor_blas.BUILD.
extern "C" void openblas_set_num_threads(int);

// Both libraries thread, and both busy-wait when idle: OpenBLAS's pool spins
// after finishing a GEMM, and OpenMP's does the same unless OMP_WAIT_POLICY is
// passive. Left alone, the pool that just ran is still burning every core when
// the next library is measured, and the numbers become a function of which
// library ran first. It shows up as a 64x64 multiply "taking" 2-4 ms.
//
// So each measurement pins the *other* library to one thread. This has to be
// done in-process rather than through OMP_NUM_THREADS / OPENBLAS_NUM_THREADS,
// because both libraries are linked into this one binary.
void use_threads_for_eigen(int n) {
  Eigen::setNbThreads(n);
  openblas_set_num_threads(1);
}

void use_threads_for_xtensor(int n) {
  Eigen::setNbThreads(1);
  openblas_set_num_threads(n);
}

// Runs `body` kRepetitions times and returns the best wall-clock time in
// milliseconds.
template <typename F> double best_ms(F body) {
  double best = 0.0;
  for (int i = 0; i < kRepetitions; i++) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    if (i == 0 || ms < best)
      best = ms;
  }
  return best;
}

// Compares a library's result against ours elementwise. Returns false and
// reports the first disagreement.
bool agrees(std::string_view label, std::string_view op, const Matrix &ours,
            const double *theirs) {
  for (std::size_t i = 0; i < ours.values().size(); i++) {
    const double a = ours.values()[i];
    const double b = theirs[i];
    const double scale = std::max({1.0, std::abs(a), std::abs(b)});
    if (std::abs(a - b) > kTolerance * scale) {
      std::fprintf(
          stderr,
          "DISAGREEMENT: %s %s at element %zu: ours=%.17g theirs=%.17g\n",
          std::string(label).c_str(), std::string(op).c_str(), i, a, b);
      return false;
    }
  }
  return true;
}

struct Row {
  std::string op;
  std::size_t n = 0;
  double ours = 0.0;
  double eigen = 0.0;
  double xtensor = 0.0;
};

// Threads each library may use when it is the one being measured. Set by
// MATRIX_OPS_BENCH_THREADS; defaults to the hardware's. This is read here
// rather than through OMP_NUM_THREADS / OPENBLAS_NUM_THREADS because both
// libraries live in this one binary and each has to be quiesced while the
// other runs -- see use_threads_for_eigen.
int bench_threads() {
  if (const char *value = std::getenv("MATRIX_OPS_BENCH_THREADS")) {
    const int n = std::atoi(value);
    if (n > 0)
      return n;
  }
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? static_cast<int>(hw) : 1;
}

// Times one operation across all three implementations, giving each its turn
// with the thread pool while the others are pinned to one thread.
template <typename FOurs, typename FEigen, typename FXtensor>
Row measure(std::string op, std::size_t n, int threads, FOurs ours,
            FEigen eigen, FXtensor xtensor) {
  Row row;
  row.op = std::move(op);
  row.n = n;

  // Ours is single-threaded, so both libraries are quiesced for its turn.
  use_threads_for_eigen(1);
  row.ours = best_ms(ours);

  use_threads_for_eigen(threads);
  row.eigen = best_ms(eigen);

  use_threads_for_xtensor(threads);
  row.xtensor = best_ms(xtensor);
  return row;
}

} // namespace

int main() {
  std::vector<Row> rows;
  bool all_agree = true;
  const int threads = bench_threads();

  for (const std::size_t n : kSizes) {
    const std::vector<double> va = random_values(n * n, 1);
    const std::vector<double> vb = random_values(n * n, 2);

    const Matrix ma = make_matrix(n, va);
    const Matrix mb = make_matrix(n, vb);
    const RowMajorXd ea = make_eigen(n, va);
    const RowMajorXd eb = make_eigen(n, vb);
    const xt::xarray<double> xa = make_xtensor(n, va);
    const xt::xarray<double> xb = make_xtensor(n, vb);

    // --- correctness, before any timing ---
    {
      const Matrix sum = *matrix_ops::add(ma, mb);
      const RowMajorXd esum = ea + eb;
      const xt::xarray<double> xsum = xa + xb;
      all_agree &= agrees("eigen", "add", sum, esum.data());
      all_agree &= agrees("xtensor", "add", sum, xsum.data());

      const Matrix diff = *matrix_ops::sub(ma, mb);
      const RowMajorXd ediff = ea - eb;
      const xt::xarray<double> xdiff = xa - xb;
      all_agree &= agrees("eigen", "sub", diff, ediff.data());
      all_agree &= agrees("xtensor", "sub", diff, xdiff.data());

      const Matrix prod = *matrix_ops::mul(ma, mb);
      const RowMajorXd eprod = ea * eb;
      const xt::xarray<double> xprod = xt::linalg::dot(xa, xb);
      all_agree &= agrees("eigen", "mul", prod, eprod.data());
      all_agree &= agrees("xtensor", "mul", prod, xprod.data());

      const Matrix scaled = matrix_ops::scale(ma, 2.5);
      const RowMajorXd escaled = ea * 2.5;
      const xt::xarray<double> xscaled = xa * 2.5;
      all_agree &= agrees("eigen", "scale", scaled, escaled.data());
      all_agree &= agrees("xtensor", "scale", scaled, xscaled.data());
    }

    // --- timings ---
    // Every result is consumed (summed into a sink) so nothing can be
    // optimized away as dead. Eigen and xtensor are lazy: without forcing the
    // expression into a concrete matrix, the "timing" would measure building
    // an expression template and nothing else.
    volatile double sink = 0.0;

    rows.push_back(measure(
        "add", n, threads,
        [&] { sink += matrix_ops::add(ma, mb)->values()[0]; },
        [&] {
          const RowMajorXd r = ea + eb;
          sink += r(0, 0);
        },
        [&] {
          const xt::xarray<double> r = xa + xb;
          sink += r(0, 0);
        }));

    rows.push_back(measure(
        "sub", n, threads,
        [&] { sink += matrix_ops::sub(ma, mb)->values()[0]; },
        [&] {
          const RowMajorXd r = ea - eb;
          sink += r(0, 0);
        },
        [&] {
          const xt::xarray<double> r = xa - xb;
          sink += r(0, 0);
        }));

    rows.push_back(measure(
        "mul", n, threads,
        [&] { sink += matrix_ops::mul(ma, mb)->values()[0]; },
        [&] {
          const RowMajorXd r = ea * eb;
          sink += r(0, 0);
        },
        [&] {
          const xt::xarray<double> r = xt::linalg::dot(xa, xb);
          sink += r(0, 0);
        }));

    rows.push_back(measure(
        "scale", n, threads,
        [&] { sink += matrix_ops::scale(ma, 2.5).values()[0]; },
        [&] {
          const RowMajorXd r = ea * 2.5;
          sink += r(0, 0);
        },
        [&] {
          const xt::xarray<double> r = xa * 2.5;
          sink += r(0, 0);
        }));
  }

  if (!all_agree) {
    std::fprintf(stderr, "\nresults disagree; timings suppressed\n");
    return 1;
  }
  std::fprintf(stderr, "all three agree on every operation and size\n\n");

  std::printf("| op | size | ours | Eigen | xtensor |\n");
  std::printf("|---|---|---|---|---|\n");
  for (const Row &row : rows) {
    std::printf("| %s | %zux%zu | %.3f ms | %.3f ms | %.3f ms |\n",
                row.op.c_str(), row.n, row.n, row.ours, row.eigen, row.xtensor);
  }
  std::printf("\nbest of %d runs, optimized build, %d thread%s per library\n",
              kRepetitions, threads, threads == 1 ? "" : "s");
  return 0;
}
