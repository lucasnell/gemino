#ifndef __JACKALOPE_MUTATOR_SUBS_H
#define __JACKALOPE_MUTATOR_SUBS_H


#include "jackalope_types.h" // integer types and debugging preprocessor directives

/*
 This defines classes for adding substitutions.
 */


#include <RcppArmadillo.h>
#include <pcg/pcg_random.hpp> // pcg prng
#include <vector>  // vector class
#include <string>  // string class


#include "var_classes.h"  // Var* classes
#include "pcg.h"  // pcg seeding
#include "alias_sampler.h"  // alias method of sampling
#include "util.h"  // str_stop



using namespace Rcpp;


// All return 4 except for TCAG
std::vector<uint8> make_char_map() {
    std::vector<uint8> out(256, 4);
    std::string bases = "TCAG";
    for (uint32 i = 0; i < 4; i++) out[bases[i]] = i;
    return out;
}



class SubMutator {

public:

    std::vector<arma::mat> Q;
    std::vector<arma::mat> U;
    std::vector<arma::mat> Ui;
    std::vector<arma::vec> L;
    double invariant;
    VarChrom* var_chrom = nullptr;  // VarChrom object pointer to be manipulated
    const std::vector<uint8> char_map = make_char_map();
    std::vector<std::vector<AliasSampler>> samplers;
    std::vector<arma::mat> Pt;
    std::deque<uint8> rate_inds;


    SubMutator() {}
    SubMutator(const std::vector<arma::mat>& Q_,
               const std::vector<arma::mat>& U_,
               const std::vector<arma::mat>& Ui_,
               const std::vector<arma::vec>& L_,
               const double& invariant_)
        : Q(Q_), U(U_), Ui(Ui_), L(L_), invariant(invariant_),
          samplers(Q_.size(), std::vector<AliasSampler>(4))),
          Pt(Q_.size(), arma::mat(4,4)) {
#ifdef __JACKALOPE_DEBUG
        if (Q_.size() == 0) stop("in SubMutator constr, Q_.size() == 0");
        if (Q_.size() > 255) stop("in SubMutator constr, Q_.size() > 255");
        if (U_.size() > 255) stop("in SubMutator constr, U_.size() > 255");
        if (Ui_.size() > 255) stop("in SubMutator constr, Ui_.size() > 255");
        if (L_.size() > 255) stop("in SubMutator constr, L_.size() > 255");
#endif
    }


    void new_chrom(VarChrom& var_chrom_, pcg64& eng);

    void add_subs(const double& b_len, pcg64& eng);


private:

    void new_branch(const double& b_len);



};






//' Changing P(t) matrix with new branch lengths or times.
//'
//'
//' Equivalent to `U %*% diag(exp(L * t)) %*% Ui`
//'
//' @noRd
//'
inline void Pt_calc(const arma::mat& U,
                    const arma::mat& Ui,
                    const arma::vec& L,
                    const double& t,
                    arma::mat& Pt) {

    arma::mat diag_L = arma::diagmat(arma::exp(L * t));

    Pt = U * diag_L * Ui;

    return;
}

//' Calculating P(t) using repeated matrix squaring, for UNREST model only.
//'
//' @noRd
//'
inline void Pt_calc(const arma::mat& Q,
                    const uint32& k,
                    const double& t,
                    arma::mat& Pt) {

    double m = static_cast<double>(1U<<k);

    Pt = arma::eye<arma::mat>(4, 4) + Q * t / m + 0.5 * (Q * t / m) * (Q * t / m);

    for (uint32 i = 0; i < k; i++) Pt = Pt * Pt;

    return;

}










#endif