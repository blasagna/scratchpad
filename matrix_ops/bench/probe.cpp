// Integration probe: the smallest program that proves Eigen, xtensor, and
// xtensor-blas all compile under this repo's -Werror settings and that
// xt::linalg::dot actually links against the system BLAS.
//
// It exists to fail fast. Delete it once //matrix_ops/bench:compare works.

#include <Eigen/Dense>
#include <xtensor-blas/xlinalg.hpp>
#include <xtensor/containers/xarray.hpp>

#include <cstdio>

int main() {
  Eigen::MatrixXd a(2, 2);
  a << 1, 2, 3, 4;
  const Eigen::MatrixXd ea = a * a;

  const xt::xarray<double> b{{1.0, 2.0}, {3.0, 4.0}};
  const xt::xarray<double> xb = xt::linalg::dot(b, b);

  // [[1 2],[3 4]]^2 == [[7 10],[15 22]]
  std::printf("eigen   %g %g %g %g\n", ea(0, 0), ea(0, 1), ea(1, 0), ea(1, 1));
  std::printf("xtensor %g %g %g %g\n", xb(0, 0), xb(0, 1), xb(1, 0), xb(1, 1));
  return 0;
}
