#include <algorithm>
#include <memory>
#include <variant>

#include <Eigen/Dense>
#include <pybind11/eigen.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

using Eigen::ComputeThinU;
using Eigen::ComputeThinV;
using Eigen::MatrixXd;
using Eigen::VectorXd;
namespace py = pybind11;

enum struct Inversion { exact = 0, subspace_exact_r = 1, subspace_re = 3 };

int calc_num_significant(const VectorXd &singular_values, double truncation) {
  int num_significant = 0;
  double total_sigma2 = singular_values.squaredNorm();

  /*
   * Determine the number of singular values by enforcing that
   * less than a fraction @truncation of the total variance be
   * accounted for.
   */
  double running_sigma2 = 0;
  for (auto sig : singular_values) {
    if (running_sigma2 / total_sigma2 <
        truncation) { /* Include one more singular value ? */
      num_significant++;
      running_sigma2 += sig * sig;
    } else
      break;
  }

  return num_significant;
}

/**
 * Implements parts of Eq. 14.31 in the book Data Assimilation,
 * The Ensemble Kalman Filter, 2nd Edition by Geir Evensen.
 * Specifically, this implements
 * X_1 (I + \Lambda_1)^{-1} X_1^T (D - M[A^f])
 */
MatrixXd genX3(const MatrixXd &W, const MatrixXd &D, const VectorXd &eig) {
  const int nrmin = std::min(D.rows(), D.cols());
  // Corresponds to (I + \Lambda_1)^{-1} since `eig` has already been
  // transformed.
  MatrixXd Lambda_inv = eig(Eigen::seq(0, nrmin - 1)).asDiagonal();
  MatrixXd X1 = Lambda_inv * W.transpose();

  MatrixXd X2 = X1 * D;
  MatrixXd X3 = W * X2;

  return X3;
}

int svdS(const MatrixXd &S, const std::variant<double, int> &truncation,
         VectorXd &inv_sig0, MatrixXd &U0) {

  int num_significant = 0;

  auto svd = S.bdcSvd(ComputeThinU);
  U0 = svd.matrixU();
  VectorXd singular_values = svd.singularValues();

  if (std::holds_alternative<int>(truncation)) {
    num_significant = std::get<int>(truncation);
  } else {
    num_significant =
        calc_num_significant(singular_values, std::get<double>(truncation));
  }

  inv_sig0 = singular_values.cwiseInverse();

  inv_sig0(Eigen::seq(num_significant, Eigen::last)).setZero();

  return num_significant;
}

/**
 Routine computes X1 and eig corresponding to Eqs 14.54-14.55
 Geir Evensen
*/
void lowrankE(
    const MatrixXd &S, /* (nrobs x nrens) */
    const MatrixXd &E, /* (nrobs x nrens) */
    MatrixXd &W, /* (nrobs x nrmin) Corresponding to X1 from Eqs. 14.54-14.55 */
    VectorXd &eig, /* (nrmin) Corresponding to 1 / (1 + Lambda1^2) (14.54) */
    const std::variant<double, int> &truncation) {

  const int nrobs = S.rows();
  const int nrens = S.cols();
  const int nrmin = std::min(nrobs, nrens);

  VectorXd inv_sig0(nrmin);
  MatrixXd U0(nrobs, nrmin);

  /* Compute SVD of S=HA`  ->  U0, invsig0=sig0^(-1) */
  svdS(S, truncation, inv_sig0, U0);

  MatrixXd Sigma_inv = inv_sig0.asDiagonal();

  /* X0(nrmin x nrens) =  Sigma0^(+) * U0'* E  (14.51)  */
  MatrixXd X0 = Sigma_inv * U0.transpose() * E;

  /* Compute SVD of X0->  U1*eig*V1   14.52 */
  auto svd = X0.bdcSvd(ComputeThinU);
  const auto &sig1 = svd.singularValues();

  /* Lambda1 = 1/(I + Lambda^2)  in 14.56 */
  for (int i = 0; i < nrmin; i++)
    eig[i] = 1.0 / (1.0 + sig1[i] * sig1[i]);

  /* Compute X1 = W = U0 * (U1=sig0^+ U1) = U0 * Sigma0^(+') * U1  (14.55) */
  W = U0 * Sigma_inv.transpose() * svd.matrixU();
}

void lowrankCinv(
    const MatrixXd &S, const MatrixXd &R,
    MatrixXd &W,   /* Corresponding to X1 from Eq. 14.29 */
    VectorXd &eig, /* Corresponding to 1 / (1 + Lambda_1) (14.29) */
    const std::variant<double, int> &truncation) {

  const int nrobs = S.rows();
  const int nrens = S.cols();
  const int nrmin = std::min(nrobs, nrens);

  MatrixXd U0(nrobs, nrmin);
  MatrixXd Z(nrmin, nrmin);

  VectorXd inv_sig0(nrmin);
  svdS(S, truncation, inv_sig0, U0);

  MatrixXd Sigma_inv = inv_sig0.asDiagonal();

  /* B = Xo = (N-1) * Sigma0^(+) * U0'* Cee * U0 * Sigma0^(+')  (14.26)*/
  MatrixXd B = (nrens - 1.0) * Sigma_inv * U0.transpose() * R * U0 *
               Sigma_inv.transpose();

  auto svd = B.bdcSvd(ComputeThinU);
  Z = svd.matrixU();
  eig = svd.singularValues();

  /* Lambda1 = (I + Lambda)^(-1) */
  for (int i = 0; i < nrmin; i++)
    eig[i] = 1.0 / (1 + eig[i]);

  Z = Sigma_inv * Z;

  W = U0 * Z; /* X1 = W = U0 * Z2 = U0 * Sigma0^(+') * Z    */
}

/**
 * Sections 3.3 and 3.4
 */
void subspace_inversion(MatrixXd &W, const Inversion ies_inversion,
                        const MatrixXd &E,
                        std::optional<py::EigenDRef<MatrixXd>> R,
                        const MatrixXd &S, const MatrixXd &H,
                        const std::variant<double, int> &truncation,
                        double ies_steplength) {
  int ens_size = S.cols();
  int nrobs = S.rows();
  double nsc = 1.0 / sqrt(ens_size - 1.0);
  MatrixXd X1 = MatrixXd::Zero(
      nrobs, std::min(ens_size, nrobs)); // Used in subspace inversion
  VectorXd eig(ens_size);

  switch (ies_inversion) {
  case Inversion::subspace_re:
    lowrankE(S, E * nsc, X1, eig, truncation);
    break;

  case Inversion::subspace_exact_r:
    lowrankCinv(S, R.value() * nsc * nsc, X1, eig, truncation);
    break;

  default:
    break;
  }

  // X3 = X1 * diag(eig) * X1' * H (Similar to Eq. 14.31, Evensen (2007))
  Eigen::Map<VectorXd> eig_vector(eig.data(), eig.size());
  MatrixXd X3 = genX3(X1, H, eig_vector);

  // (Line 9)
  W = ies_steplength * S.transpose() * X3 + (1.0 - ies_steplength) * W;
}

/**
 * Section 3.2 - Exact inversion assuming diagonal error covariance matrix
 */
void exact_inversion(MatrixXd &W, const MatrixXd &S, const MatrixXd &H,
                     double ies_steplength) {
  int ens_size = S.cols();

  MatrixXd C = S.transpose() * S;
  C.diagonal().array() += 1;

  auto svd = C.bdcSvd(Eigen::ComputeFullV);

  W = W - ies_steplength *
              (W - svd.matrixV() *
                       svd.singularValues().cwiseInverse().asDiagonal() *
                       svd.matrixV().transpose() * S.transpose() * H);
}

/**
 * @brief Computer coefficient matrix (W) following steps 4-8
 * of Algorithm 1.
 *
 * W = W - ies_steplength * (W - S' * (S * S' + R)^{-1} * H)
 * When R=I Line 9 can be rewritten as
 * W = W - ies_steplength * ( W - (S'*S + I)^{-1} * S' * H )
 * Notice the expression being inverted.
 * Instead of S * S' which is a (num_obs, num_obs) sized matrix,
 * we get S' * S which is of size (ensemble_size, ensemble_size).
 * This is great since num_obs is usually much larger than ensemble_size.
 *
 * @param Y Predicted ensemble anomalies normalized by sqrt(N-1),
 *          where N is the number of realizations.
 *          See line 4 of Algorithm 1 and Eq. 30.
 */
MatrixXd create_coefficient_matrix(py::EigenDRef<MatrixXd> Y,
                                   std::optional<py::EigenDRef<MatrixXd>> R,
                                   py::EigenDRef<MatrixXd> E,
                                   py::EigenDRef<MatrixXd> D,
                                   const Inversion ies_inversion,
                                   const std::variant<double, int> &truncation,
                                   MatrixXd &W, double ies_steplength)

{
  const int ens_size = Y.cols();

  /* Line 5 of Algorithm 1 */
  MatrixXd Omega =
      (1.0 / sqrt(ens_size - 1.0)) * (W.colwise() - W.rowwise().mean());
  Omega.diagonal().array() += 1.0;

  /* Solving for the average sensitivity matrix.
     Line 6 of Algorithm 1, also Section 5
  */
  Omega.transposeInPlace();
  MatrixXd S = Omega.fullPivLu().solve(Y.transpose()).transpose();

  /* Similar to the innovation term.
     Differs in that `D` here is defined as dobs + E - Y instead of just dobs +
     E as in the paper. Line 7 of Algorithm 1, also Section 2.6
  */
  MatrixXd H = D + S * W;

  /*
   * With R=I the subspace inversion (ies_inversion=1) with
   * singular value trucation=1.000 gives exactly the same solution as the exact
   * inversion (`ies_inversion`=Inversion::exact).
   *
   * With very large data sets it is likely that the inversion becomes poorly
   * conditioned and a trucation=1.0 is not a good choice. In this case
   * `ies_inversion` other than Inversion::exact and truncation set to less
   * than 1.0 could stabilize the algorithm.
   */

  if (ies_inversion == Inversion::exact) {
    exact_inversion(W, S, H, ies_steplength);
  } else {
    subspace_inversion(W, ies_inversion, E, R, S, H, truncation,
                       ies_steplength);
  }

  return W;
}

MatrixXd makeD(const VectorXd &obs_values, const MatrixXd &E,
               const MatrixXd &S) {

  MatrixXd D = E - S;

  D.colwise() += obs_values;

  return D;
}

PYBIND11_MODULE(_ies, m) {
  using namespace py::literals;

  m.def("create_coefficient_matrix", &create_coefficient_matrix, "Y0"_a,
        "R"_a = py::none(), "E"_a, "D"_a, "ies_inversion"_a, "truncation"_a,
        "W"_a, "ies_steplength"_a);
  m.def("make_D", &makeD, "obs_values"_a, "E"_a, "S"_a);

  py::enum_<Inversion>(m, "InversionType")
      .value("EXACT", Inversion::exact)
      .value("EXACT_R", Inversion::subspace_exact_r)
      .value("SUBSPACE_RE", Inversion::subspace_re)
      .export_values();
}
