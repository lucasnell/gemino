#ifndef __GEMINO_MEVO_H
#define __GEMINO_MEVO_H


#include <RcppArmadillo.h>
#include <pcg/pcg_random.hpp> // pcg prng
#include <vector>  // vector class
#include <string>  // string class
#include <random>  // gamma_distribution


#include "gemino_types.h" // integer types
#include "sequence_classes.h"  // Var* and Ref* classes
#include "pcg.h"  // pcg seeding
#include "table_sampler.h"  // table method of sampling
#include "mevo_gammas.h"  // SequenceGammas class
#include "weighted_reservoir.h"  // weighted_reservoir_* functions



using namespace Rcpp;

namespace mevo {
    const std::string bases = "TCAG";
}


/*
 =========================================================================================
 =========================================================================================
 =========================================================================================
 =========================================================================================

 Choosing mutation locations based on overall mutation rates that vary by
 (i) nucleotide and (ii) sequence region

 =========================================================================================
 =========================================================================================
 =========================================================================================
 =========================================================================================
 */






/*
 Stores info on the overall mutation rates (including Gammas) for each nucleotide.

 This class is used to do weighted reservoir sampling for where a mutation should occur.
 Ultimately this class allows you to use a bracket operator to get a rate for a
 given location in the variant genome.

 For nucleotide rates (not including Gammas), Ns are set to 0 bc we don't want to
 process these.
 Input char objects are cast to uint32 which provide the indices.
 T, C, A, G, and N should never be higher than 84, so will be safe.
 In case someone inputs other characters accidentally, I've set the length to 256,
 which should work for all 8-bit character values.
 The memory overhead should be pretty minimal.
 */

class MutationRates {

public:

    const VarSequence * vs;  // pointer to const VarSequence
    std::vector<double> nt_rates;
    SequenceGammas gammas;


    MutationRates() : vs(), nt_rates(), gammas() {}

    /*
     Below `q_tcag` is a length-4 vector of rates (q_i from Yang (2006)), for
     T, C, A, and G, respectively
     */
    MutationRates(const VarSequence& vs_, const std::vector<double>& q_tcag,
                  const SequenceGammas& gammas_)
        : vs(&vs_), nt_rates(256, 0.0), gammas(gammas_) {
        for (uint32 i = 0; i < 4; i++) {
            uint32 j = mevo::bases[i];
            nt_rates[j] = q_tcag[i];
        }
    }
    MutationRates(const std::vector<double>& q_tcag):
        vs(), nt_rates(256, 0.0), gammas() {
        for (uint32 i = 0; i < 4; i++) {
            uint32 j = mevo::bases[i];
            nt_rates[j] = q_tcag[i];
        }
    }
    MutationRates(const MutationRates& other)
        : vs(other.vs), nt_rates(other.nt_rates), gammas(other.gammas) {}

    MutationRates& operator=(const MutationRates& other) {
        vs = other.vs;
        nt_rates = other.nt_rates;
        gammas = other.gammas;
        return *this;
    }


    // To get size of the variant sequence
    inline uint32 size() const noexcept {
        // If a null pointer, return 0
        if (!vs) return 0;
        return vs->size();
    }

    // Using bracket operator to get the overall mutation rate at a location
    inline double operator[](const uint32& pos) const {
        char c = vs->get_nt(pos);
        double r = nt_rates[c];
        r *= gammas[pos];
        return r;
    }

    /*
     The same as above, but for a range of positions.
     */
    inline double operator()(const uint32& start, const uint32& end) const {

        std::string seq;
        uint32 mut_ = vs->get_mut_(start);
        vs->set_seq_chunk(seq, start, end - start + 1, mut_);

        std::vector<double> gamma_vals = gammas(start, end);
        if (gamma_vals.size() != seq.size()) {
            stop("seq and gamma_vals sizes not matching in MutationRates::().");
        }

        double out = 0;
        for (uint32 i = 0; i < gamma_vals.size(); i++) {
            double r = nt_rates[seq[i]];
            out += r * gamma_vals[i];
        }

        return out;
    }

    /*
     Get the change in mutation rate for a substitution at a location given a
     position and the character it'll change to
     */
    inline double sub_rate_change(const uint32& pos, const char& c) const {
        char c0 = vs->get_nt(pos);
        double gamma = gammas[pos];
        double r0 = nt_rates[c0];
        double r1 = nt_rates[c];
        return gamma * (r1 - r0);
    }

    // Return a rate (NO gamma) for an input string
    inline double raw_rate(const std::string& seq) const {
        double out = 0;
        for (const char& c : seq) out += nt_rates[c];
        return out;
    }
    // Overloaded for a character
    inline double raw_rate(const char& seq) const {
        double out = nt_rates[seq];
        return out;
    }

    // Used below to check if gamma region needs to be iterated to the next one.
    inline void check_gamma(const uint32& pos,
                            uint32& gamma_end, uint32& gam_i, double& gamma,
                            const SequenceGammas& gammas) const {
        if (pos > gamma_end) {
            gam_i++;
            gamma = gammas.regions[gam_i].gamma;
            gamma_end = gammas.regions[gam_i].end;
        }
        return;
    }

    // To return the overall rate for an entire sequence:
    double total_rate(uint32 start, uint32 end, const bool& ranged) const {

        double out = 0;

        if (!ranged) {
            start = 0;
            end = vs->size() - 1;
        }

        if ((vs->size() - 1) != gammas.regions.back().end) {
            stop("gammas and vs sizes don't match inside MutationRates");
        }

        // If there are no mutations, this is pretty easy:
        if (vs->mutations.empty()) {

            if ((vs->ref_seq.nucleos.size() - 1) != gammas.regions.back().end) {
                stop("gammas and vs ref sizes don't match inside MutationRates");
            }

            uint32 i = start, gam_i = gammas.get_idx(start);

            while (i <= end) {
                double gamma = gammas.regions[gam_i].gamma;
                double tmp = 0;
                while (i <= gammas.regions[gam_i].end && i <= end) {
                    tmp += nt_rates[vs->ref_seq.nucleos[i]];
                    i++;
                }
                out += (tmp * gamma);
                gam_i++;
            }

            return out;
        }

        // Index to the first Mutation object not past `start` position:
        uint32 mut_i = vs->get_mut_(start);
        // Index to the corresponding gamma region:
        uint32 gam_i = gammas.get_idx(start);

        double gamma = gammas.regions[gam_i].gamma;
        uint32 gamma_end = gammas.regions[gam_i].end;

        // Current position
        uint32 pos = start;

        /*
         If `start` is before the first mutation (resulting in
         `mut_i == vs->mutations.size()`),
         we must pick up any nucleotides before the first mutation.
         */
        if (mut_i == vs->mutations.size()) {
            mut_i = 0;
            for (; pos < vs->mutations[mut_i].new_pos; pos++) {
                check_gamma(pos, gamma_end, gam_i, gamma, gammas);
                out += (nt_rates[vs->ref_seq[pos]] * gamma);
            }
            check_gamma(pos, gamma_end, gam_i, gamma, gammas);
        }


        /*
         Now, for each subsequent mutation except the last, add all nucleotides
         at or after its position but before the next one.
         I'm adding `pos <= end` inside all while-statement checks to make sure
         it doesn't keep going after we've reached `end`.
         */
        uint32 next_mut_i = mut_i + 1;
        while (pos <= end && next_mut_i < vs->mutations.size()) {
            while (pos <= end && pos < vs->mutations[next_mut_i].new_pos) {
                char c = vs->get_char_(pos, mut_i);
                out += nt_rates[c] * gamma;
                ++pos;
                check_gamma(pos, gamma_end, gam_i, gamma, gammas);
            }
            ++mut_i;
            ++next_mut_i;
        }

        // Now taking care of nucleotides after the last Mutation
        while (pos <= end &&pos < vs->seq_size) {
            char c = vs->get_char_(pos, mut_i);
            out += nt_rates[c] * gamma;
            ++pos;
            check_gamma(pos, gamma_end, gam_i, gamma, gammas);
        }

        return out;
    }

};


/*
 This class uses the info above, plus a class and fxn from `weighted_reservoir.h` to
 do weighted reservoir sampling for a single location at which to put a mutation.
 The weights are based on the nucleotide and sequence region.

 Class `C` should be `ReservoirRates` or `ChunkReservoirRates`.

 If using `ReservoirRates`, this samples the entire sequence, which can be
 inefficient for large sequences.

 If using `ChunkReservoirRates`, this samples a set number of sequence locations
 at a time instead of the whole thing.
 It extracts a "chunk" for weighted sampling by using non-weighted sampling without
 replacement.
 For large sequences, this is much more efficient and, from my testing, produces
 similar results.
 */
template <template <typename> class C>
class OneSeqLocationSampler {

public:

    C<MutationRates> rates;

    OneSeqLocationSampler() : rates() {};
    OneSeqLocationSampler(const MutationRates& mr_, const uint32& chunk)
        : rates(mr_, chunk) {}
    OneSeqLocationSampler(const OneSeqLocationSampler<C>& other)
        : rates(other.rates) {}
    OneSeqLocationSampler<C>& operator=(const OneSeqLocationSampler<C>& other) {
        rates = other.rates;
        return *this;
    }


    inline uint32 sample(pcg32& eng, const uint32& start, const uint32& end,
                         const bool& ranged) {
        return rates.sample(eng, start, end, ranged);
    }

};



/*
 Simplifying names and adding the following functionality:
 - return rate changes for given substitutions, insertions, or deletions
 - return rate of the whole sequence with `total_rate()` method
 - change gamma region bounds with the `update_gamma_regions()` method
 */
class LocationSampler: public OneSeqLocationSampler<ReservoirRates> {
public:

    // Constructors:
    LocationSampler() : OneSeqLocationSampler<ReservoirRates>() {}
    LocationSampler(const MutationRates& mr_)
        : OneSeqLocationSampler<ReservoirRates>(mr_, 0) {};
    // Copy constructor
    LocationSampler(const LocationSampler& other)
        : OneSeqLocationSampler<ReservoirRates>(other) {};
    // Assignment operator
    LocationSampler& operator=(const LocationSampler& other) {
        OneSeqLocationSampler<ReservoirRates>::operator=(other);
        return *this;
    }

    /*
     Return reference to inner MutationRates field.
     */
    inline MutationRates& mr() {
        return rates.res_rates;
    }
    // const version
    inline const MutationRates& mr() const {
        return rates.res_rates;
    }

    /*
     Get the change in mutation rate for a substitution at a location given a
     position and the character it'll change to
     */
    double substitution_rate_change(const char& c, const uint32& pos) const {
        const MutationRates& mr_(mr());
        return mr_.sub_rate_change(pos, c);
    }
    /*
     Get the change in mutation rate for an insertion at a location given a
     position and the characters that'll be inserted.
     */
    double insertion_rate_change(const std::string& seq, const uint32& pos) const {
        const MutationRates& mr_(mr());
        double gamma = mr_.gammas[pos];
        double rate = mr_.raw_rate(seq);
        return gamma * rate;
    }
    /*
     Get the change in mutation rate for a deletion at a location given a
     position and the deletion size.
     */
    double deletion_rate_change(const sint32& size_mod, const uint32& start) const {
        uint32 end = start - size_mod - 1;
        const MutationRates& mr_(mr());
        double out = mr_(start, end);
        out *= -1;
        return out;
    }
    /*
     Return the total rate for a VarSequence object
    */
    inline double total_rate(const uint32& start, const uint32& end,
                             const bool& ranged) const {
        const MutationRates& mr_(mr());
        return mr_.total_rate(start, end, ranged);
    }
    /*
     To update gamma boundaries when indels occur:
     */
    inline void update_gamma_regions(const sint32& size_change, const uint32& pos) {
        MutationRates& mr_(mr());
        mr_.gammas.update(pos, size_change);
        return;
    }

};

class ChunkLocationSampler: public OneSeqLocationSampler<ChunkReservoirRates> {
public:


    // Constructors:
    ChunkLocationSampler() : OneSeqLocationSampler<ChunkReservoirRates>() {}
    ChunkLocationSampler(const MutationRates& mr_, const uint32 chunk = 0)
        : OneSeqLocationSampler<ChunkReservoirRates>(mr_, chunk) {}
    // Copy constructor
    ChunkLocationSampler(const ChunkLocationSampler& other)
        : OneSeqLocationSampler<ChunkReservoirRates>(other) {};
    // Assignment operator
    ChunkLocationSampler& operator=(const ChunkLocationSampler& other) {
        OneSeqLocationSampler<ChunkReservoirRates>::operator=(other);
        return *this;
    }

    inline MutationRates& mr() {
        return rates.res_rates.all_rates;
    }
    inline const MutationRates& mr() const {
        return rates.res_rates.all_rates;
    }

    double substitution_rate_change(const char& c, const uint32& pos) const {
        const MutationRates& mr_(mr());
        return mr_.sub_rate_change(pos, c);
    }

    double insertion_rate_change(const std::string& seq, const uint32& pos) const {
        const MutationRates& mr_(mr());
        double gamma = mr_.gammas[pos];
        double rate = mr_.raw_rate(seq);
        return gamma * rate;
    }

    double deletion_rate_change(const sint32& size_mod, const uint32& start) const {
        uint32 end = start - size_mod - 1;
        const MutationRates& mr_(mr());
        double out = mr_(start, end);
        out *= -1;
        return out;
    }

    inline double total_rate(const uint32& start, const uint32& end,
                             const bool& ranged) const {
        const MutationRates& mr_(mr());
        return mr_.total_rate(start, end, ranged);
    }

    inline void update_gamma_regions(const sint32& size_change, const uint32& pos) {
        MutationRates& mr_(mr());
        mr_.gammas.update(pos, size_change);
        return;
    }


    // Resize chunk size; this method is obviously not available for non-chunked version
    void change_chunk(const uint32& chunk_size) {
        ChunkRateGetter<MutationRates>& crg(rates.res_rates);
        crg.chunk_size = chunk_size;
        if (crg.all_rates.size() > chunk_size) {
            crg.inds.resize(chunk_size);
        } else if (crg.all_rates.size() != crg.inds.size()) {
            crg.inds.resize(crg.all_rates.size());
        }
        // `recheck_size_()` will automatically do the rest of the work from here
        return;
    }
};









/*
 =========================================================================================
 =========================================================================================
 =========================================================================================
 =========================================================================================

 Choosing mutation type based on the starting nucleotide

 =========================================================================================
 =========================================================================================
 =========================================================================================
 =========================================================================================
 */



/*
 For a single mutation's info
 */
struct MutationInfo {
    char nucleo;
    sint32 length;

    MutationInfo() : nucleo(), length() {}
    MutationInfo(const MutationInfo& other)
        : nucleo(other.nucleo), length(other.length) {}
    MutationInfo& operator=(const MutationInfo& other) {
        nucleo = other.nucleo;
        length = other.length;
        return *this;
    }

    // Initialize from an index and mut-lengths vector
    MutationInfo (const uint32& ind, const std::vector<sint32>& mut_lengths)
        : nucleo('\0'), length(0) {
        if (ind < 4) {
            nucleo = mevo::bases[ind];
        } else {
            length = mut_lengths[ind];
        }
    }
};



/*
 For constructors, this creates a vector of indices for each char in "TCAG" (0 to 3).
 Because char objects can be easily cast to uints, I can input a char from a sequence
 and get out an index to which TableSampler object to sample from.
 This way is much faster than using an unordered_map.
 Using 8-bit uints bc the char should never be >= 256.
 It's only of length 85 (versus 256 in MutationRates class) because characters other
 than T, C, A, or G have rates hard-coded to zero and should never be chosen.
 */
inline std::vector<uint8> make_base_inds() {
    std::vector<uint8> base_inds(85);
    uint8 i = 0;
    for (const char& c : mevo::bases) {
        base_inds[c] = i;
        i++;
    }
    return base_inds;
}



/*
 For table-sampling mutation types depending on which nucleotide you start with.
 The `mut_lengths` vector tells how long each mutation is.
 This field is 0 for substitions, < 0 for deletions, and > 0 for insertions.
 The `base_inds` field allows me to convert the characters 'T', 'C', 'A', or 'G'
 (cast to uints) into uints from 0 to 3.
 */
class MutationTypeSampler {

    std::vector<TableSampler> sampler;
    std::vector<sint32> mut_lengths;
    std::vector<uint8> base_inds;

public:

    MutationTypeSampler() : sampler(4), mut_lengths(), base_inds() {
        base_inds = make_base_inds();
    }
    MutationTypeSampler(const std::vector<std::vector<double>>& probs,
                        const std::vector<sint32>& mut_lengths_)
    : sampler(4), mut_lengths(mut_lengths_), base_inds() {
        base_inds = make_base_inds();
        if (probs.size() != 4) stop("probs must be size 4.");
        for (uint32 i = 0; i < 4; i++) {
            std::vector<double> probs_i = probs[i];
            /*
             Item `i` in `probs_i` needs to be manually converted to zero so
             it's not sampled.
             (Remember, we want the probability of each, *given that a mutation occurs*.
             Mutating into itself doesn't count.)
            */
            probs_i[i] = 0;
            // Now create and fill the `TableSampler`:
            sampler[i] = TableSampler(probs_i);
        }
    }
    // copy constructor
    MutationTypeSampler(const MutationTypeSampler& other)
        : sampler(other.sampler), mut_lengths(other.mut_lengths),
          base_inds(other.base_inds) {}
    // Assignment operator
    MutationTypeSampler& operator=(const MutationTypeSampler& other) {
        sampler = other.sampler;
        mut_lengths = other.mut_lengths;
        base_inds = other.base_inds;
        return *this;
    }

    /*
     Sample a mutation based on an input nucleotide.
     `c` gets cast to an uint32, which is then input to `base_inds` to get the index
     from 0 to 3.
     */
    MutationInfo sample(const char& c, pcg32& eng) const {
        uint32 ind = sampler[base_inds[c]].sample(eng);
        MutationInfo mi(ind, mut_lengths);
        return mi;
    }

};









/*
 =========================================================================================
 =========================================================================================
 =========================================================================================
 =========================================================================================

 Combining samplers for location and for mutation type into a mutation sampler for
 a single sequence.

 =========================================================================================
 =========================================================================================
 =========================================================================================
 =========================================================================================
 */



/*
 OneSeqMutationSampler combines objects for sampling mutation types and new
 nucleotides for insertions.

 Class `C` should be `LocationSampler` or `ChunkLocationSampler`.
 */
template <class C>
class OneSeqMutationSampler {

    /*
     Sample for mutation location based on rates by sequence region and nucleotide,
     for the whole sequence or a range.
     */
    inline uint32 sample_location(pcg32& eng,
                                  const uint32& start = 0, const uint32& end = 0,
                                  const bool& ranged = false) {
        return location.sample(eng, start, end, ranged);
    }

    /*
    Sample for mutation type based on nucleotide and rng engine
    */
    inline MutationInfo sample_type(const char& c, pcg32& eng) const {
        return type.sample(c, eng);
    }

    /*
    Create a new string of nucleotides (for insertions) of a given length and using
    an input rng engine
    */
    inline std::string new_nucleos(const uint32& len, pcg32& eng) const {
        std::string str(len, 'x');
        insert.sample(str, eng);
        return str;
    }

public:

    // VarSequence object pointer to be manipulated
    VarSequence* vs;
    // For sampling the mutation location:
    C location;
    // For sampling the type of mutation:
    MutationTypeSampler type;
    // For new insertion sequences:
    TableStringSampler<std::string> insert;

    OneSeqMutationSampler() : vs(), location(), type(), insert() {}

    OneSeqMutationSampler(VarSequence& vs_,
                          const C& location_,
                          const MutationTypeSampler& type_,
                          const TableStringSampler<std::string>& insert_)
        : vs(&vs_), location(location_), type(type_), insert(insert_) {}

    OneSeqMutationSampler(const OneSeqMutationSampler<C>& other)
        : vs(other.vs), location(other.location), type(other.type),
          insert(other.insert) {}

    OneSeqMutationSampler<C>& operator=(const OneSeqMutationSampler<C>& other) {
        if (other.vs) vs = other.vs;
        location = other.location;
        type = other.type;
        insert = other.insert;
        return *this;
    }

    void fill_ptrs(VarSequence& vs_) {
        vs = &vs_;
        location.mr().vs = &vs_;
        return;
    }

    void fill_gamma(const arma::mat& gamma_mat) {
        location.mr().gammas = SequenceGammas(gamma_mat);
        return;
    }

    // Add mutation and return the change in the sequence rate that results
    double mutate(pcg32& eng) {
        uint32 pos = sample_location(eng);
        char c = vs->get_nt(pos);
        MutationInfo m = sample_type(c, eng);
        double rate_change;
        if (m.length == 0) {
            rate_change = location.substitution_rate_change(m.nucleo, pos);
            vs->add_substitution(m.nucleo, pos);
        } else {
            if (m.length > 0) {
                std::string nts = new_nucleos(m.length, eng);
                rate_change = location.insertion_rate_change(nts, pos);
                vs->add_insertion(nts, pos);
            } else {
                sint64 pos_ = static_cast<sint64>(pos);
                sint64 size_ = static_cast<sint64>(vs->size());
                if (pos_ - m.length > size_) m.length = static_cast<sint32>(pos_-size_);
                uint32 del_size = std::abs(m.length);
                rate_change = location.deletion_rate_change(m.length, pos);
                vs->add_deletion(del_size, pos);
            }
            // Update Gamma region bounds:
            location.update_gamma_regions(m.length, pos);

        }
        return rate_change;
    }

    /*
     Overloaded for only mutating within a range.
     It also updates `end` if an indel occurs in the range.
     Make sure to keep checking for situation where `end < start` (i.e., sequence section
     is empty).
     `// ***` mark difference between this and previous `mutate` versions
     */
    double mutate(pcg32& eng, const uint32& start, sint64& end) {
        uint32 pos = sample_location(eng, start, static_cast<uint32>(end), true);  // ***
        char c = vs->get_nt(pos);
        MutationInfo m = sample_type(c, eng);
        double rate_change;
        if (m.length == 0) {
            rate_change = location.substitution_rate_change(m.nucleo, pos);
            vs->add_substitution(m.nucleo, pos);
        } else {
            if (m.length > 0) {
                std::string nts = new_nucleos(m.length, eng);
                rate_change = location.insertion_rate_change(nts, pos);
                vs->add_insertion(nts, pos);
            } else {
                sint64 pos_ = static_cast<sint64>(pos);
                sint64 size_ = end + 1;  // ***
                if (pos_ - m.length > size_) m.length = static_cast<sint32>(pos_-size_);
                uint32 del_size = std::abs(m.length);
                rate_change = location.deletion_rate_change(m.length, pos);
                vs->add_deletion(del_size, pos);
            }
            // Update Gamma region bounds:
            location.update_gamma_regions(m.length, pos);
            // Update end point:
            end += static_cast<sint64>(m.length);  // ***
        }
        return rate_change;
    }

    double total_rate(const uint32& start = 0, const uint32& end = 0,
                      const bool& ranged = false) {
        return location.total_rate(start, end, ranged);
    }
};


// Shortening these names
typedef OneSeqMutationSampler<LocationSampler> MutationSampler;
typedef OneSeqMutationSampler<ChunkLocationSampler> ChunkMutationSampler;




void fill_mut_prob_length_vectors(
        std::vector<std::vector<double>>& probs,
        std::vector<sint32>& mut_lengths,
        const arma::mat& Q,
        const double& xi,
        const double& psi,
        const std::vector<double>& pi_tcag,
        arma::vec rel_insertion_rates,
        arma::vec rel_deletion_rates);


MutationSampler make_mutation_sampler(VarSequence& vs,
                                      const std::vector<std::vector<double>>& probs,
                                      const std::vector<sint32>& mut_lengths,
                                      const std::vector<double>& pi_tcag,
                                      const arma::mat& gamma_mat);


ChunkMutationSampler make_mutation_sampler(VarSequence& vs,
                                           const std::vector<std::vector<double>>& probs,
                                           const std::vector<sint32>& mut_lengths,
                                           const std::vector<double>& pi_tcag,
                                           const arma::mat& gamma_mat,
                                           const uint32& chunk_size);



#endif
