#ifndef READ_PAIR
#define READ_PAIR

extern "C" {
#include "sam.h"
}

#include "SailfishMath.hpp"
#include "LibraryFormat.hpp"

struct ReadPair {
    bam1_t* read1;
    bam1_t* read2;
    double logProb;

    inline char* getName() {
        return bam1_qname(read1);
    }

    inline uint32_t fragLen() {
        std::abs(read1->core.pos - read2->core.pos) + read2->core.l_qseq;
    }

    inline bool isRight() { return false; }
    inline bool isLeft()  { return false; }

    inline int32_t left() { return std::min(read1->core.pos, read2->core.pos); }
    inline int32_t right() {
        return std::max(read1->core.pos + read1->core.l_qseq,
                        read2->core.pos + read2->core.l_qseq);
    }

    inline ReadType fragType() { return ReadType::PAIRED_END; }
    inline int32_t transcriptID() { return read1->core.tid; }

    inline double logQualProb() {
        auto q1 = read1->core.qual;
        auto q2 = read2->core.qual;
        //double logP1 = (q1 == 255) ? calcQuality(read1) : std::log(std::pow(10.0, -q1 * 0.1));
        //double logP2 = (q2 == 255) ? calcQuality(read2) : std::log(std::pow(10.0, -q2 * 0.1));
        double logP1 = (q1 == 255) ? sailfish::math::LOG_1 : std::log(std::pow(10.0, -q1 * 0.1));
        double logP2 = (q2 == 255) ? sailfish::math::LOG_1 : std::log(std::pow(10.0, -q2 * 0.1));
        return logP1 + logP2;
    }
};

#endif //READ_PAIR
