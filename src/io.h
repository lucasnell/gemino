#ifndef __JACKAL_READ_WRITE_H
#define __JACKAL_READ_WRITE_H

/*
 Functions common to reading/writing to multiple file types.
 */

#include <RcppArmadillo.h>
#include <vector>               // vector class
#include <string>               // string class

#include <fstream>
#include "zlib.h"


#include "htslib/bgzf.h"  // BGZF

#include "jackalope_types.h"  // integer types
#include "seq_classes_ref.h"  // Ref* classes
#include "seq_classes_var.h"  // Var* classes



using namespace Rcpp;




// Size of the block of memory to use for reading non-indexed fasta files and fai files.
#define LENGTH 0x1000 // hexadecimel for 4096.




inline void expand_path(std::string& file_name) {
    // Obtain environment containing function
    Environment base("package:base");
    // Make function callable from C++
    Function pe_r = base["path.expand"];
    // Call the function and receive its list output
    SEXP fai_file_exp = pe_r(file_name);
    file_name = as<std::string>(fai_file_exp);
    return;
}



// Simple wrapper aroung BGZF class to have `write` and `close` methods
struct FileBGZF {

    BGZF *file;

    FileBGZF() {};

    FileBGZF(const std::string& out_prefix,
             const int& n_threads,
             const int& compress) {

        if (compress < -1 || compress > 9) {
            str_stop({"\nInvalid bgzip compress level of ",
                     std::to_string(compress),
                     ". It must be in range [0,9]."});
        }
        char out_mode[3] = "w\0";
        if (compress >= 0) out_mode[1] = compress + '0';

        char *name = new char[out_prefix.size() + 5];
        strcpy(name, out_prefix.c_str());
        strcat(name, ".gz");
        file = bgzf_open(name, out_mode);
        if (file == NULL) {
            delete [] name;
            str_stop({"\nIn bgzip step, it can't create ", out_prefix, ".gz"});
        }
        delete [] name;

        if (n_threads > 1) bgzf_mt(file, n_threads, 256);

    }
    // Serial version
    FileBGZF(const std::string& out_prefix,
             const int& compress) {

        if (compress < -1 || compress > 9) {
            str_stop({"\nInvalid bgzip compress level of ",
                     std::to_string(compress),
                     ". It must be in range [0,9]."});
        }
        std::string out_mode= "w";
        if (compress >= 0) out_mode += std::to_string(compress);

        char *name = new char[out_prefix.size() + 5];
        strcpy(name, out_prefix.c_str());
        strcat(name, ".gz");
        file = bgzf_open(name, out_mode.c_str());
        if (file == NULL) {
            delete [] name;
            str_stop({"\nIn bgzip step, it can't create ", out_prefix, ".gz"});
        }
        delete [] name;

    }

    // Allows to set after initializing blank
    void set(const std::string& out_prefix,
             const int& compress) {
        *this = FileBGZF(out_prefix, compress);
        return;
    }


    inline void write(void *buffer, const int& c) {
        code = bgzf_write(file, buffer, c);
        return;
    }
    inline void write(const std::vector<char>& buffer) {
        code = bgzf_write(file, buffer.data(), buffer.size());
        return;
    }
    inline void write(const std::string& buffer) {
        code = bgzf_write(file, buffer.c_str(), buffer.size());
        return;
    }

    int close() {
        code = bgzf_close(file);
        if (code < 0) str_warn({"Close failed: Error ", std::to_string(file->errcode)});
        return code;
    }


private:

    int code;

};

// Simple wrapper around gzfile class to have `write` and `close` methods
struct FileGZ {

    gzFile file;

    FileGZ() {};

    FileGZ(const std::string& out_prefix,
               const int& compress) {

        if (compress < 0 || compress > 9) {
            str_stop({"\nInvalid bgzip compress level of ",
                     std::to_string(compress),
                     ". It must be in range [0,9]."});
        }
        char out_mode[3] = "w\0";
        out_mode[1] = compress + '0';

        std::string file_name = out_prefix + ".gz";

        // Initialize file.
        // Note that gzfile does not tolerate initializing an empty file.
        // Use ofstream instead.
        if (!std::ifstream(file_name)){
            std::ofstream myfile;
            myfile.open(file_name, std::ios::out | std::ios::binary);
            myfile.close();
        }

        file = gzopen(file_name.c_str(), "wb");
        if (!file) {
            str_stop({"gzopen of ", file_name, " failed: ", strerror(errno), ".\n"});
        }
    }

    // Allows to set after initializing blank
    void set(const std::string& out_prefix,
             const int& compress) {
        *this = FileGZ(out_prefix, compress);
        return;
    }


    inline void write(const std::vector<char>& buffer) {
        code = gzwrite(file, buffer.data(), buffer.size());
        return;
    }
    inline void write(const std::string& buffer) {
        code = gzwrite(file, buffer.c_str(), buffer.size());
        return;
    }

    int close() {
        code = gzclose(file);
        /*
         Success.
         */
        if (code == Z_OK) {
            code = 0;
        /*
         Some type of error.
         */
        } else {
            /*
             file was NULL (or Z_NULL), or did not refer to an open compressed
             file stream.
             */
            if (code == Z_STREAM_ERROR) {
                code = -1;
            /*
             An error occurred in the underlying base libraries, and the
             application should check errno for further information.
             */
            } else if (code == Z_ERRNO) {
                code = -2;
            /*
             no compression progress is possible during buffer flush (see deflate()).
             */
            } else if (code == Z_BUF_ERROR) {
                code = -3;
            }
        }
        return code;
    }


private:
    int code;

};



// Simple wrapper around std::ofstream class to have `write` and `close` methods
// that are exactly the same as FileGZ and FileBGZF above
struct FileUncomp {

    std::ofstream file;

    FileUncomp() {};

    FileUncomp(const std::string& file_name) {
        construct(file_name, 0);
    }
    /*
     The compress argument is added here for compatibility with templates that
     allow compressed-file classes.
     */
    FileUncomp(const std::string& file_name,
               const int& compress) {
        construct(file_name, compress);
    }

    // Allows to set after initializing blank
    void set(const std::string& file_name,
             const int& compress) {
        construct(file_name, compress);
        return;
    }

    inline void write(const std::vector<char>& buffer) {
        file.write(buffer.data(), buffer.size());
        return;
    }
    inline void write(const std::string& buffer) {
        file.write(buffer.c_str(), buffer.size());
        return;
    }

    int close() {
        file.close();
        return 0;
    }


private:

    void construct(const std::string& file_name,
                   const int& compress) {

        file.open(file_name, std::ofstream::out);

        if (!file.is_open()) {
            str_stop({"Unable to open file ", file_name, ".\n"});
        }

        return;
    }

};
















#endif
