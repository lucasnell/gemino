
/*
 ****************************************************

 Functions for testing molecular evolution code.
 These functions are R versions of what would normally be run entirely from C++.
 This is to test the output in R using the testthat package.

 ****************************************************
 */


#include <RcppArmadillo.h>
#include <cmath>  // pow, log, exp
#include <pcg/pcg_random.hpp> // pcg prng
#include <vector>  // vector class
#include <string>  // string class
#include <progress.hpp>  // for the progress bar



#include "gemino_types.h"
#include "mevo.h"
#include "sequence_classes.h"  // Var* and Ref* classes
#include "pcg.h"  // pcg seeding
#include "table_sampler.h"  // table method of sampling
#include "weighted_reservoir.h"  // weighted reservoir sampling
#include "mevo_gammas.h"  // SequenceGammas class
#include "mevo_phylo.h"  // match_ and template functions
#include "mevo_rate_matrices.h"  // rate matrix functions

using namespace Rcpp;



//' Test sampling based on a evolutionary model.
//'
//' @noRd
//'
//[[Rcpp::export]]
void test_sampling(SEXP& vs_sexp, const uint32& N,
                   const std::vector<double>& pi_tcag,
                   const double& alpha_1, const double& alpha_2,
                   const double& beta,
                   const double& xi, const double& psi,
                   const arma::vec& rel_insertion_rates,
                   const arma::vec& rel_deletion_rates,
                   arma::mat gamma_mat,
                   const uint32& chunk_size,
                   bool display_progress = true) {

    XPtr<VarSet> vs_xptr(vs_sexp);
    VarSet& vs_(*vs_xptr);
    VarSequence& vs(vs_[0][0]);

    arma::mat Q = TN93_rate_matrix(pi_tcag, alpha_1, alpha_2, beta, xi);

    std::vector<std::vector<double>> probs;
    std::vector<sint32> mut_lengths;

    fill_mut_prob_length_vectors(probs, mut_lengths, Q, xi, psi, pi_tcag,
                                 rel_insertion_rates, rel_deletion_rates);

    pcg32 eng = seeded_pcg();

    ChunkMutationSampler ms = make_mutation_sampler(vs, probs, mut_lengths, pi_tcag,
                                                    gamma_mat, chunk_size);

    Progress p(N, display_progress);

    for (uint32 i = 0; i < N; i++) {
        if (Progress::check_abort()) return;
        p.increment(); // update progress
        if (vs.size() == 0) return;
        // Mutating and ignoring the rate change that it outputs:
        static_cast<void>(ms.mutate(eng));
    }

    return;
}




// Turn a Mutation into a List
List conv_mut(const Mutation& mut) {
    List out = List::create(_["size_modifier"] = mut.size_modifier,
                            _["old_pos"] = mut.old_pos,
                            _["new_pos"] = mut.new_pos,
                            _["nucleos"] = mut.nucleos);
    return out;
}



//' Turns a VarGenome's mutations into a list of data frames.
//'
//' Temporary function for testing.
//'
//'
//' @noRd
//'
//[[Rcpp::export]]
List see_mutations(SEXP vs_, const uint32& var_ind) {

    XPtr<VarSet> vs(vs_);
    VarGenome& vg((*vs)[var_ind]);

    List out(vg.size());
    for (uint32 i = 0; i < vg.size(); i++) {
        const VarSequence& vs(vg.var_genome[i]);
        std::vector<sint32> size_mod;
        std::vector<uint32> old_pos;
        std::vector<uint32> new_pos;
        std::vector<std::string> nucleos;
        for (uint32 mut_i = 0; mut_i < vs.mutations.size(); ++mut_i) {
            size_mod.push_back(vs.mutations[mut_i].size_modifier);
            old_pos.push_back(vs.mutations[mut_i].old_pos);
            new_pos.push_back(vs.mutations[mut_i].new_pos);
            nucleos.push_back(vs.mutations[mut_i].nucleos);
        }
        DataFrame mutations_i = DataFrame::create(
            _["size_mod"] = size_mod,
            _["old_pos"] = old_pos,
            _["new_pos"] = new_pos,
            _["nucleos"] = nucleos);
        out[i] = mutations_i;
    }
    return out;
}


//' Turns a VarGenome's mutations into a list of data frames.
//'
//' Temporary function for testing.
//'
//'
//' @noRd
//'
//[[Rcpp::export]]
List examine_mutations(SEXP var_set_sexp, const uint32& var_ind, const uint32& seq_ind) {

    XPtr<VarSet> var_set_xptr(var_set_sexp);
    const VarGenome& vg((*var_set_xptr)[var_ind]);
    const VarSequence& vs(vg[seq_ind]);

    std::string bases = "TCAG";
    std::vector<uint32> base_inds(85);
    uint32 j = 0;
    for (const char& c : bases) {
        base_inds[static_cast<uint32>(c)] = j;
        j++;
    }

    uint32 n_muts = vs.mutations.size();
    arma::mat sub_mat(4, 4, arma::fill::zeros);
    uint32 max_ins = 0;
    uint32 max_del = 0;
    for (uint32 i = 0; i < n_muts; i++) {
        sint32 mi = vs.mutations[i].size_modifier;
        if (mi == 0) continue;
        if (mi > 0) {
            if (mi > max_ins) max_ins = mi;
        } else {
            uint32 mid = static_cast<uint32>(std::abs(mi));
            if (mid > max_del) max_del = mid;
        }
    }
    arma::mat ins_mat(4, max_ins, arma::fill::zeros);
    arma::mat del_mat(4, max_del, arma::fill::zeros);
    std::vector<uint32> pos_vec(n_muts);

    for (uint32 mut_i = 0; mut_i < n_muts; mut_i++) {

        const Mutation& m(vs.mutations[mut_i]);

        char c = vs.ref_seq[m.old_pos];
        uint32 i = base_inds[static_cast<uint32>(c)];
        sint32 smod = m.size_modifier;
        if (smod == 0) {
            uint32 j = base_inds[static_cast<uint32>(m.nucleos[0])];
            sub_mat(i, j)++;
        } else if (smod > 0) {
            uint32 j = static_cast<uint32>(smod - 1);
            ins_mat(i, j)++;
        } else {
            uint32 j = static_cast<uint32>(std::abs(smod + 1));
            del_mat(i, j)++;
        }

        pos_vec[mut_i] = vs.mutations[mut_i].old_pos;
    }

    List out = List::create(
        _["sub"] = wrap(sub_mat),
        _["ins"] = wrap(ins_mat),
        _["del"] = wrap(del_mat),
        _["pos"] = pos_vec);

    return out;
}


//' Faster version of table function to count the number of mutations in Gamma regions.
//'
//'
//'
//[[Rcpp::export]]
std::vector<uint32> table_gammas(const std::vector<uint32>& gamma_ends,
                               const std::vector<uint32>& positions) {
    std::vector<uint32> out(gamma_ends.size(), 0U);
    for (uint32 i = 0; i < positions.size(); i++) {
        uint32 j = std::lower_bound(gamma_ends.begin(), gamma_ends.end(),
                                  positions[i]) - gamma_ends.begin();
        out[j]++;
    }
    return out;
}



//' Add mutations manually from R.
//'
//' Note that all indices are in 0-based C++ indexing. This means that the first
//' item is indexed by `0`, and so forth.
//'
//' @param vs_ External pointer to a C++ `VarSet` object
//' @param var_ind Integer index to the desired variant. Uses 0-based indexing!
//' @param seq_ind Integer index to the desired sequence. Uses 0-based indexing!
//' @param new_pos_ Integer index to the desired subsitution location.
//'     Uses 0-based indexing!
//'
//' @name add_mutations
NULL_ENTRY;

//' @describeIn add_mutations Add a substitution.
//'
//' @inheritParams vs_ add_mutations
//' @inheritParams var_ind add_mutations
//' @inheritParams seq_ind add_mutations
//' @param nucleo_ Character to substitute for existing one.
//' @inheritParams new_pos_ add_mutations
//'
//'
//[[Rcpp::export]]
void add_substitution(SEXP vs_, const uint32& var_ind,
                      const uint32& seq_ind,
                      const char& nucleo_,
                      const uint32& new_pos_) {
    XPtr<VarSet> vset(vs_);
    VarGenome& vg((*vset)[var_ind]);
    VarSequence& vs(vg[seq_ind]);
    vs.add_substitution(nucleo_, new_pos_);
    return;
}
//' @describeIn add_mutations Add an insertion.
//'
//' @inheritParams vs_ add_mutations
//' @inheritParams var_ind add_mutations
//' @inheritParams seq_ind add_mutations
//' @param nucleos_ Nucleotides to insert at the desired location.
//' @inheritParams new_pos_ add_mutations
//'
//'
//[[Rcpp::export]]
void add_insertion(SEXP vs_, const uint32& var_ind,
                   const uint32& seq_ind,
                   const std::string& nucleos_,
                   const uint32& new_pos_) {
    XPtr<VarSet> vset(vs_);
    VarGenome& vg((*vset)[var_ind]);
    VarSequence& vs(vg[seq_ind]);
    vs.add_insertion(nucleos_, new_pos_);
    return;
}
//' @describeIn add_mutations Add a deletion.
//'
//' @inheritParams vs_ add_mutations
//' @inheritParams var_ind add_mutations
//' @inheritParams seq_ind add_mutations
//' @param size_ Size of deletion.
//' @inheritParams new_pos_ add_mutations
//'
//'
//[[Rcpp::export]]
void add_deletion(SEXP vs_, const uint32& var_ind,
                  const uint32& seq_ind,
                  const uint32& size_,
                  const uint32& new_pos_) {
    XPtr<VarSet> vset(vs_);
    VarGenome& vg((*vset)[var_ind]);
    VarSequence& vs(vg[seq_ind]);
    vs.add_deletion(size_, new_pos_);
    return;
}




//' Get a rate for given start and end points of a VarSequence.
//'
//' @noRd
//'
//[[Rcpp::export]]
double test_rate(const uint32& start, const uint32& end,
                 const uint32& var_ind, const uint32& seq_ind,
                 SEXP var_set_sexp, SEXP sampler_sexp) {

    XPtr<VarSet> var_set(var_set_sexp);
    VarSequence& vs((*var_set)[var_ind][seq_ind]);

    XPtr<ChunkMutationSampler> sampler(sampler_sexp);

    arma::mat gamma_mat(1, 2);
    gamma_mat(0,0) = vs.size();
    gamma_mat(0,1) = 1;

    sampler->fill_ptrs(vs);
    sampler->fill_gamma(gamma_mat);

    double out = sampler->total_rate(start, end, true);

    return out;

}






//' Test sampling based on an evolutionary model.
//'
//' Make SURE `sampler_base_sexp` is a `ChunkMutationSampler`, not a `MutationSampler`!
//'
//' @param tip_labels Character vector of the actual phylogeny's tip labels.
//' @param ordered_tip_labels Character vector of the tip labels in the order
//'     you want them.
//'
//' @return A vector of integers indicating the number of mutations per edge.
//'
//' @noRd
//'
//[[Rcpp::export]]
std::vector<uint32> test_phylo(SEXP& vs_sexp,
                               SEXP& sampler_base_sexp,
                               const uint32& seq_ind,
                               const std::vector<double>& branch_lens,
                               arma::Mat<uint32> edges,
                               const std::vector<std::string>& tip_labels,
                               const std::vector<std::string>& ordered_tip_labels,
                               const arma::mat& gamma_mat,
                               const bool& recombination = false,
                               const uint32& start = 0,
                               const sint64& end = 0) {

    XPtr<VarSet> vs_xptr(vs_sexp);
    XPtr<ChunkMutationSampler> sampler_base_xptr(sampler_base_sexp);

    Progress progress(100, false);

    if (gamma_mat(gamma_mat.n_rows-1,0) != (*vs_xptr)[0][seq_ind].size()) {
        stop("gamma_mat doesn't reach the end of the sequence.");
    }

    uint32 n_tips = vs_xptr->size();
    if (ordered_tip_labels.size() != n_tips || tip_labels.size() != n_tips) {
        stop("ordered_tip_labels and tip_labels must have the same length as ",
             "# variants.");
    }

    std::vector<uint32> spp_order = match_(ordered_tip_labels, tip_labels);

    uint32 n_edges = edges.n_rows;
    if (branch_lens.size() != n_edges) {
        stop("branch_lens must have the same length as the # rows in edges.");
    }
    if (edges.n_cols != 2) stop("edges must have exactly two columns.");

    std::vector<uint32> n_muts(n_edges, 0);

    // From R to C++ indices
    edges -= 1;

    pcg32 eng = seeded_pcg();

    int code = one_tree_<ChunkMutationSampler>(
        *vs_xptr, *sampler_base_xptr, seq_ind, branch_lens, edges, spp_order,
        gamma_mat, eng, progress, n_muts, recombination, start, end
    );

    // Make sure this happens outside of multithreaded code
    if (code == -1) {
        std::string warn_msg = "\nUser interrupted phylogenetic evolution. ";
        warn_msg += "Note that changes occur in place, so your variants have ";
        warn_msg += "already been partially added.";
        // throw(Rcpp::exception(err_msg.c_str(), false));
        Rcpp::warning(warn_msg.c_str());
    }

    return n_muts;
}







