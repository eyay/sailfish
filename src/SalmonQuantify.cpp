/**
>HEADER
    Copyright (c) 2013 Rob Patro robp@cs.cmu.edu

    This file is part of Sailfish.

    Sailfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Sailfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Sailfish.  If not, see <http://www.gnu.org/licenses/>.
<HEADER
**/


#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <unordered_map>
#include <map>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <sstream>
#include <exception>
#include <random>
#include <queue>
#include "btree_map.h"
#include "btree_set.h"


/** Boost Includes */
#include <boost/filesystem.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/range/irange.hpp>
#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/thread/thread.hpp>

#include "tbb/concurrent_unordered_set.h"
#include "tbb/concurrent_vector.h"
#include "tbb/concurrent_unordered_map.h"
#include "tbb/concurrent_queue.h"
#include "tbb/parallel_for.h"
#include "tbb/parallel_for_each.h"
#include "tbb/parallel_reduce.h"
#include "tbb/blocked_range.h"
#include "tbb/task_scheduler_init.h"
#include "tbb/partitioner.h"

#if HAVE_LOGGER
#include "g2logworker.h"
#include "g2log.h"
#endif

#include "cereal/types/vector.hpp"
#include "cereal/archives/binary.hpp"

#include "jellyfish/mer_dna.hpp"

#include "ClusterForest.hpp"
#include "PerfectHashIndex.hpp"
#include "LookUpTableUtils.hpp"
#include "SailfishMath.hpp"
#include "Transcript.hpp"
#include "LibraryFormat.hpp"
#include "SailfishUtils.hpp"
#include "ReadLibrary.hpp"

#include "PairSequenceParser.hpp"

#include "google/dense_hash_map"

typedef pair_sequence_parser<char**> sequence_parser;

using TranscriptID = uint32_t;
using TranscriptIDVector = std::vector<TranscriptID>;
using KmerIDMap = std::vector<TranscriptIDVector>;
using my_mer = jellyfish::mer_dna_ns::mer_base_static<uint64_t, 1>;

uint32_t transcript(uint64_t enc) {
    uint32_t t = (enc & 0xFFFFFFFF00000000) >> 32;
    return t;
}

uint32_t offset(uint64_t enc) {
    uint32_t o = enc & 0xFFFFFFFF;
    return o;
}

class Alignment {
    public:
        Alignment(TranscriptID transcriptIDIn, uint32_t kCountIn = 1, double logProbIn = sailfish::math::LOG_0) :
            transcriptID_(transcriptIDIn), kmerCount(kCountIn), logProb(logProbIn) {}

        inline TranscriptID transcriptID() { return transcriptID_; }
        uint32_t kmerCount;
        double logProb;

    private:
        TranscriptID transcriptID_;
};

void processMiniBatch(
        double logForgettingMass,
        std::vector<std::vector<Alignment>>& batchHits,
        std::vector<Transcript>& transcripts,
        ClusterForest& clusterForest
        ) {

    using sailfish::math::LOG_0;
    using sailfish::math::logAdd;
    using sailfish::math::logSub;

    size_t numTranscripts{transcripts.size()};

    bool burnedIn{true};
    // Build reverse map from transcriptID => hit id
    using HitID = uint32_t;
    btree::btree_map<TranscriptID, std::vector<Alignment*>> hitsForTranscript;
    size_t hitID{0};
    for (auto& hv : batchHits) {
        for (auto& tid : hv) {
            hitsForTranscript[tid.transcriptID()].push_back(&tid);
        }
        ++hitID;
    }

    double clustTotal = std::log(batchHits.size()) + logForgettingMass;
    {
        // E-step

        // Iterate over each group of alignments (a group consists of all alignments reported
        // for a single read).  Distribute the read's mass proportionally dependent on the
        // current hits
        for (auto& alnGroup : batchHits) {
            if (alnGroup.size() == 0) { continue; }
            double sumOfAlignProbs{LOG_0};
            // update the cluster-level properties
            bool transcriptUnique{true};
            auto firstTranscriptID = alnGroup.front().transcriptID();
            std::unordered_set<size_t> observedTranscripts;
            for (auto& aln : alnGroup) {
                auto transcriptID = aln.transcriptID();
                auto& transcript = transcripts[transcriptID];
                transcriptUnique = transcriptUnique and (transcriptID == firstTranscriptID);

                double refLength = transcript.RefLength > 0 ? transcript.RefLength : 1.0;

                double logFragProb = sailfish::math::LOG_1;

                // The alignment probability is the product of a transcript-level term (based on abundance and) an alignment-level
                // term below which is P(Q_1) * P(Q_2) * P(F | T)
                double logRefLength = std::log(refLength);
                double transcriptLogCount = transcript.mass();
                if ( transcriptLogCount != LOG_0 ) {
                    double errLike = sailfish::math::LOG_1;
                    if (burnedIn) {
                        //errLike = errMod.logLikelihood(aln, transcript);
                    }

                    aln.logProb = std::log(std::pow(aln.kmerCount,2.0)) + (transcriptLogCount - logRefLength);// + qualProb + errLike;
                    //aln.logProb = (transcriptLogCount - logRefLength);// + qualProb + errLike;


                    sumOfAlignProbs = logAdd(sumOfAlignProbs, aln.logProb);
                    if (observedTranscripts.find(transcriptID) == observedTranscripts.end()) {
                        transcripts[transcriptID].addTotalCount(1);
                        observedTranscripts.insert(transcriptID);
                    }
                } else {
                    aln.logProb = LOG_0;
                }
            }

            // normalize the hits
            if (sumOfAlignProbs == LOG_0) { std::cerr << "0 probability fragment; skipping\n"; continue; }
            for (auto& aln : alnGroup) {
                aln.logProb -= sumOfAlignProbs;
                auto transcriptID = aln.transcriptID();
                auto& transcript = transcripts[transcriptID];
                /*
                double r = uni(eng);
                if (!burnedIn and r < std::exp(aln.logProb)) {
                    errMod.update(aln, transcript, aln.logProb, logForgettingMass);
                    if (aln.fragType() == ReadType::PAIRED_END) {
                        double fragLength = aln.fragLen();//std::abs(aln.read1->core.pos - aln.read2->core.pos) + aln.read2->core.l_qseq;
                        fragLengthDist.addVal(fragLength, logForgettingMass);
                    }
                }
                */

            } // end normalize

            // update the single target transcript
            if (transcriptUnique) {
                transcripts[firstTranscriptID].addUniqueCount(1);
                clusterForest.updateCluster(firstTranscriptID, 1, logForgettingMass);
            } else { // or the appropriate clusters
                clusterForest.mergeClusters<Alignment>(alnGroup.begin(), alnGroup.end());
                clusterForest.updateCluster(alnGroup.front().transcriptID(), 1, logForgettingMass);
            }

            } // end read group
        }// end timer

        double individualTotal = LOG_0;
        {
            // M-step
            double totalMass{0.0};
            for (auto kv = hitsForTranscript.begin(); kv != hitsForTranscript.end(); ++kv) {
                auto transcriptID = kv->first;
                // The target must be a valid transcript
                if (transcriptID >= numTranscripts or transcriptID < 0) {std::cerr << "index " << transcriptID << " out of bounds\n"; }

                auto& transcript = transcripts[transcriptID];

                // The prior probability
                double hitMass{LOG_0};

                // The set of alignments that match transcriptID
                auto& hits = kv->second;
                std::for_each(hits.begin(), hits.end(), [&](Alignment* aln) -> void {
                        if (!std::isfinite(aln->logProb)) { std::cerr << "hitMass = " << aln->logProb << "\n"; }
                        hitMass = logAdd(hitMass, aln->logProb);
                        });

                double updateMass = logForgettingMass + hitMass;
                individualTotal = logAdd(individualTotal, updateMass);
                totalMass = logAdd(totalMass, updateMass);
                transcript.addMass(updateMass);
            } // end for
        } // end timer
        //if (processedReads >= 5000000 and !burnedIn) { burnedIn = true; }
}

uint32_t basesCovered(std::vector<uint32_t>& kmerHits) {
    std::sort(kmerHits.begin(), kmerHits.end());
    uint32_t covered{0};
    uint32_t lastHit{0};
    uint32_t kl{20};
    for (auto h : kmerHits) {
        covered += std::min(h - lastHit, kl);
        lastHit = h;
    }
    return covered;
}

uint32_t basesCovered(std::vector<uint32_t>& posLeft, std::vector<uint32_t>& posRight) {
    return basesCovered(posLeft) + basesCovered(posRight);
}

class KmerVote {
    public:
        KmerVote(uint32_t vp, uint32_t rp) : votePos(vp), readPos(rp) {}
        uint32_t votePos{0};
        uint32_t readPos{0};
};


class TranscriptHitList {
    public:
        uint32_t bestHitPos{0};
        uint32_t bestHitScore{0};
        std::vector<KmerVote> votes;

        void addVote(uint32_t tpos, uint32_t readPos) {
            uint32_t transcriptPos = (readPos > tpos) ? 0 : tpos - readPos;
            votes.emplace_back(transcriptPos, readPos);
        }

        void addVoteRC(uint32_t tpos, uint32_t readPos) {
            uint32_t transcriptPos = (readPos > tpos) ? 0 : tpos + readPos;
            votes.emplace_back(transcriptPos, readPos);
        }


        uint32_t totalNumHits() { return votes.size(); }

        bool computeBestHit() {
            std::sort(votes.begin(), votes.end(),
                    [](const KmerVote& v1, const KmerVote& v2) -> bool {
                        if (v1.votePos == v2.votePos) {
                            return v1.readPos < v2.readPos;
                        }
                        return v1.votePos < v2.votePos;
                    });

            /*
            std::cerr << "(" << votes.size() << ") { ";
            for (auto v : votes) {
                std::cerr << v.votePos << " ";
            }
            std::cerr << "}\n";
            */
            uint32_t maxClusterPos{0};
            uint32_t maxClusterCount{0};
            uint32_t klen{20};
            struct VoteInfo {
                uint32_t coverage;
                uint32_t rightmostBase;
            };

            boost::container::flat_map<uint32_t, VoteInfo> hitMap;
            int32_t currClust{votes.front().votePos};
            for (size_t j = 0; j < votes.size(); ++j) {

                uint32_t votePos = votes[j].votePos;
                uint32_t readPos = votes[j].readPos;

                if (votePos >= currClust) {
                    if (votePos - currClust > 10) {
                        currClust = votePos;
                    }
                    auto& hmEntry = hitMap[currClust];
                    hmEntry.coverage += std::min(klen, (readPos + klen) - hmEntry.rightmostBase);
                    hmEntry.rightmostBase = readPos + klen;
                } else if (votePos < currClust) {
                    std::cerr << "CHA?!?\n";
                    if (currClust - votePos > 10) {
                        currClust = votePos;
                    }
                    auto& hmEntry = hitMap[currClust];
                    hmEntry.coverage += std::min(klen, (readPos + klen) - hmEntry.rightmostBase);
                    hmEntry.rightmostBase = readPos + klen;
                }

                if (hitMap[currClust].coverage > maxClusterCount) {
                    maxClusterCount = hitMap[currClust].coverage;
                    maxClusterPos = currClust;
                }

            }

            bestHitPos = maxClusterPos;
            bestHitScore = maxClusterCount;
            return true;
        }
};

// To use the parser in the following, we get "jobs" until none is
// available. A job behaves like a pointer to the type
// jellyfish::sequence_list (see whole_sequence_parser.hpp).
void add_sizes(sequence_parser* parser, std::atomic<uint64_t>* total_fwd, std::atomic<uint64_t>* total_bwd,
               std::atomic<uint64_t>& totalHits,
               std::atomic<uint64_t>& rn,
               PerfectHashIndex& phi,
               std::vector<Transcript>& transcripts,
               std::atomic<uint64_t>& batchNum,
               double& logForgettingMass,
               std::mutex& ffMutex,
               ClusterForest& clusterForest,
               std::vector<uint64_t>& offsets,
               std::vector<uint64_t>& kmerLocs,
               google::dense_hash_map<uint64_t, uint64_t>& khash
               //std::vector<std::vector<uint64_t>>& kmerLocMap
               ) {
  uint64_t count_fwd = 0, count_bwd = 0;
  auto INVALID = phi.INVALID;
  auto merLen = phi.kmerLength();

  double forgettingFactor{0.65};

  std::vector<std::vector<Alignment>> hitLists;
  hitLists.resize(5000);

  uint64_t leftHitCount{0};
  uint64_t hitListCount{0};

  class DirHit {
      uint32_t fwd{0};
      uint32_t rev{0};
  };

  size_t locRead{0};
  while(true) {
    sequence_parser::job j(*parser); // Get a job from the parser: a bunch of read (at most max_read_group)
    if(j.is_empty()) break;          // If got nothing, quit

    my_mer kmer;
    my_mer rkmer;

    for(size_t i = 0; i < j->nb_filled; ++i) { // For all the read we got

        hitLists.resize(j->nb_filled);

        /*
        btree::btree_map<uint64_t, uint32_t> eqLeftFwdPos;
        btree::btree_map<uint64_t, uint32_t> eqLeftBwdPos;
        btree::btree_map<uint64_t, uint32_t> eqRightFwdPos;
        btree::btree_map<uint64_t, uint32_t> eqRightBwdPos;
        */

        std::unordered_map<uint64_t, TranscriptHitList> leftFwdHits;
        std::unordered_map<uint64_t, TranscriptHitList> leftBwdHits;

        std::unordered_map<uint64_t, TranscriptHitList> rightFwdHits;
        std::unordered_map<uint64_t, TranscriptHitList> rightBwdHits;

        uint32_t totFwdLeft = 0;
        uint32_t totBwdLeft = 0;
        uint32_t totFwdRight = 0;
        uint32_t totBwdRight = 0;
        uint32_t readLength = 0;

        uint32_t leftReadLength{0};
        uint32_t rightReadLength{0};

        uint64_t prevEQ{std::numeric_limits<uint64_t>::max()};

        count_fwd += j->data[i].first.seq.size();        // Add up the size of the sequence
        count_bwd += j->data[i].second.seq.size();        // Add up the size of the sequence

        //---------- Left End ----------------------//
        {
            const char* start     = j->data[i].first.seq.c_str();
            uint32_t readLen      = j->data[i].first.seq.size();
            leftReadLength = readLen;
            const char* const end = start + readLen;
            uint32_t cmlen{0};
            uint32_t rbase{0};
            // iterate over the read base-by-base
            while(start < end) {
                ++rbase; ++readLength;
                char base = *start; ++start;
                auto c = jellyfish::mer_dna::code(base);
                kmer.shift_left(c);
                rkmer.shift_right(jellyfish::mer_dna::complement(c));
                switch(c) {
                    case jellyfish::mer_dna::CODE_IGNORE:
                        break;
                    case jellyfish::mer_dna::CODE_COMMENT:
                        std::cerr << "ERROR: unexpected character " << base << " in read!\n";
                        // Fall through
                    case jellyfish::mer_dna::CODE_RESET:
                        cmlen = 0;
                        kmer.polyA(); rkmer.polyA();
                        break;

                    default:
                        if(++cmlen >= merLen) {
                            cmlen = merLen;
                            auto merID = phi.index(kmer.get_bits(0, 2*merLen));
                            //auto merIt = khash.find(kmer.get_bits(0, 2*merLen));
                            //auto merID = (merIt == khash.end()) ? INVALID : merIt->second;


                            if (merID != INVALID) {
                                // Locations
                                auto first = offsets[merID];
                                auto last = offsets[merID+1];
                                for (size_t ei = first; ei < last; ++ei) {
                                    uint64_t e = kmerLocs[ei];
                                    //for (uint64_t e : kmerLocMap[merID]) {
                                    leftFwdHits[transcript(e)].addVote(offset(e), rbase - merLen);
                                }
                                /*
                                auto eqClass = memberships[merID];
                                eqLeftFwdPos[eqClass]++;//.push_back(rbase);
                                */
                                totFwdLeft++;
                            }

                            auto rmerID = phi.index(rkmer.get_bits(0, 2*merLen));
                            //auto rmerIt = khash.find(rkmer.get_bits(0, 2*merLen));
                            //auto rmerID = (rmerIt == khash.end()) ? INVALID : rmerIt->second;

                            if (rmerID != INVALID) {
                                // Locations
                                auto first = offsets[rmerID];
                                auto last = offsets[rmerID+1];
                                for (size_t ei = first ; ei < last; ++ei) {
                                    uint64_t e = kmerLocs[ei];
                                    //for (uint64_t e : kmerLocMap[rmerID]) {
                                    leftFwdHits[transcript(e)].addVoteRC(offset(e), rbase - merLen);
                                }

                                /*
                                auto eqClass = memberships[rmerID];
                                eqLeftBwdPos[eqClass]++;//.push_back(rbase);
                                */
                                totBwdLeft++;
                            }

                        }

                        } // end switch(c)
                } // while (start < end)
        }

        hitLists[i].clear();
        auto& hitList = hitLists[i];
        prevEQ = std::numeric_limits<uint64_t>::max();
        //---------- Right End ----------------------//
        {
            kmer.polyA();
            rkmer.polyA();
            const char* start     = j->data[i].second.seq.c_str();
            uint32_t readLen      = j->data[i].second.seq.size();
            rightReadLength = readLen;
            const char* const end = start + readLen;
            uint32_t cmlen{0};
            uint32_t rbase{0};

            // iterate over the read base-by-base
            while(start < end) {
                ++rbase; ++readLength;
                char base = *start; ++start;
                auto c = jellyfish::mer_dna::code(base);
                kmer.shift_left(c);
                rkmer.shift_right(jellyfish::mer_dna::complement(c));
                switch(c) {
                    case jellyfish::mer_dna::CODE_IGNORE:
                        break;
                    case jellyfish::mer_dna::CODE_COMMENT:
                        std::cerr << "ERROR: unexpected character " << base << " in read!\n";
                        // Fall through
                    case jellyfish::mer_dna::CODE_RESET:
                        cmlen = 0;
                        kmer.polyA(); rkmer.polyA();
                        break;

                    default:
                        if(++cmlen >= merLen) {
                            cmlen = merLen;
                            auto merID = phi.index(kmer.get_bits(0, 2*merLen));
                            //auto merIt = khash.find(kmer.get_bits(0, 2*merLen));
                            //auto merID = (merIt == khash.end()) ? INVALID : merIt->second;


                            if (merID != INVALID) {
                                // Locations
                                auto first = offsets[merID];
                                auto last = offsets[merID+1];
                                for (size_t ei = first; ei < last; ++ei) {
                                    uint64_t e = kmerLocs[ei];
                                    //for (uint64_t e : kmerLocMap[merID]) {
                                     rightFwdHits[transcript(e)].addVote(offset(e), rbase - merLen);
                                }
                                /*
                                auto eqClass = memberships[merID];
                                eqRightFwdPos[eqClass]++;//.push_back(rbase);
                                */
                                totFwdRight++;

                            }


                            auto rmerID = phi.index(rkmer.get_bits(0, 2*merLen));
                            //auto rmerIt = khash.find(rkmer.get_bits(0, 2*merLen));
                            //auto rmerID = (rmerIt == khash.end()) ? INVALID : rmerIt->second;

                            if (rmerID != INVALID) {
                                // Locations
                                auto first = offsets[rmerID];
                                auto last = offsets[rmerID+1];
                                for (size_t ei = first; ei < last; ++ei) {
                                    uint64_t e = kmerLocs[ei];
                                    //for (uint64_t e : kmerLocMap[rmerID]) {
                                    rightFwdHits[transcript(e)].addVoteRC(offset(e), rbase - merLen);
                                }
                                /*
                                auto eqClass = memberships[rmerID];
                                eqRightBwdPos[eqClass]++;//.push_back(rbase);
                                */
                                totBwdRight++;
                            }



                        }

                } // end switch(c)
            } // while (start < end)
        }

        /*
        std::unordered_map<TranscriptID, uint32_t> leftHits;
        std::unordered_map<TranscriptID, uint32_t> rightHits;

       auto& eqLeftPos = (totFwdLeft >= totBwdLeft) ? eqLeftFwdPos : eqLeftBwdPos;
       auto& eqRightPos = (totFwdRight >= totBwdRight) ? eqRightFwdPos : eqRightBwdPos;

      for (auto& leqPos: eqLeftPos) {
          for (auto t : klut[leqPos.first]) {
              // --- counts
              leftHits[t] += leqPos.second;//.size();

              // --- positions
             //auto& thits = leftHits[t];
             //thits.insert(thits.end(), leqPos.second.begin(), leqPos.second.end());
         }
      }

      for (auto& reqPos : eqRightPos) {
          for (auto t : klut[reqPos.first]) {
              auto it = leftHits.find(t);
              if (it != leftHits.end() and it->second > 40) {
                  // --- counts
                  rightHits[t] += reqPos.second;//.size();

                  // --- positions
                  //auto score = basesCovered(it->second);
                  //if (score < 70) { continue; }
                  //auto& thits = rightHits[t];
                  //thits.insert(thits.end(), reqPos.second.begin(), reqPos.second.end());
              }
          }
      }

      uint32_t readHits{0};
      for (auto& rightHit : rightHits) {
          if (rightHit.second > 40) {
              // --- counts
              uint32_t score = leftHits[rightHit.first] + rightHit.second;
              hitList.emplace_back(rightHit.first, score);
              readHits += score;

              // --- positions
              //auto rscore = basesCovered(rightHit.second);
               //if (rscore >= 70) {
              //uint32_t score = leftHits[rightHit.first] + rightHit.second;
              //hitList.emplace_back(rightHit.first, score);
              //readHits += score;
          }
      }
      leftHitCount += leftHits.size();
      hitListCount += hitList.size();

        */

        size_t readHits{0};
    std::unordered_map<TranscriptID, uint32_t> leftHitCounts;
    std::unordered_map<TranscriptID, uint32_t> rightHitCounts;

    auto& leftHits = leftFwdHits;//(totFwdLeft >= totBwdLeft) ? leftFwdHits : leftBwdHits;
    auto& rightHits = rightFwdHits;//(totFwdRight >= totBwdRight) ? rightFwdHits : rightBwdHits;

    for (auto& tHitList : leftHits) {
        // Coverage score
        tHitList.second.computeBestHit();
        //leftHitCounts[tHitList.first] = tHitList.second.totalNumHits();
        ++leftHitCount;
    }

    double cutoffLeft{ 0.80 * leftReadLength};
    double cutoffRight{ 0.80 * rightReadLength};

    double cutoffLeftCount{ 0.80 * leftReadLength - merLen + 1};
    double cutoffRightCount{ 0.80 * rightReadLength - merLen + 1};
    for (auto& tHitList : rightHits) {
        auto it = leftHits.find(tHitList.first);
        // Simple counting score
        /*
        if (it != leftHits.end() and it->second.totalNumHits() >= cutoffLeftCount) {
            if (tHitList.second.totalNumHits() < cutoffRightCount) { continue; }
            uint32_t score = it->second.totalNumHits() + tHitList.second.totalNumHits();
        */
        // Coverage score
        if (it != leftHits.end() and it->second.bestHitScore >= cutoffLeft) {
            tHitList.second.computeBestHit();
            if (tHitList.second.bestHitScore < cutoffRight) { continue; }
            uint32_t score = it->second.bestHitScore + tHitList.second.bestHitScore;


            hitList.emplace_back(tHitList.first, score);
            readHits += score;
            ++hitListCount;
        }
    }


      totalHits += hitList.size() > 0;
      locRead++;
      ++rn;
      if (rn % 50000 == 0) {
          std::cerr << "\r\rprocessed read "  << rn;
          std::cerr << "\n leftHits.size() " << leftHits.size()
                    << ", rightHits.size() " << rightHits.size() << ", hit list of size = " << hitList.size() << "\n";
          std::cerr << "average leftHits = " << leftHitCount / static_cast<float>(locRead)
                    << ", average hitList = " << hitListCount / static_cast<float>(locRead) << "\n";
      }

      if (hitList.size() > 100) { hitList.clear(); }

      double invHits = 1.0 / readHits;
      for (auto t : hitList) {
          transcripts[t.transcriptID()].addSharedCount( t.kmerCount * invHits );
      }

    } // end for i < j->nb_filled

    auto oldBatchNum = batchNum++;
    if (oldBatchNum > 1) {
        ffMutex.lock();
        logForgettingMass += forgettingFactor * std::log(static_cast<double>(oldBatchNum-1)) -
        std::log(std::pow(static_cast<double>(oldBatchNum), forgettingFactor) - 1);
        ffMutex.unlock();
    }

    //
    processMiniBatch(logForgettingMass, hitLists, transcripts, clusterForest);

  }

  *total_fwd += count_fwd;
  *total_bwd += count_bwd;
}

int salmonQuantify(int argc, char *argv[]) {
    using std::cerr;
    using std::vector;
    using std::string;
    namespace bfs = boost::filesystem;
    namespace po = boost::program_options;

    uint32_t maxThreads = std::thread::hardware_concurrency();

    vector<string> unmatedReadFiles;
    vector<string> mate1ReadFiles;
    vector<string> mate2ReadFiles;

    po::options_description generic("salmon quant options");
    generic.add_options()
    ("version,v", "print version string")
    ("help,h", "produce help message")
    ("index,i", po::value<string>(), "sailfish index.")
    ("libtype,l", po::value<std::string>(), "Format string describing the library type")
    ("unmated_reads,r", po::value<vector<string>>(&unmatedReadFiles)->multitoken(),
     "List of files containing unmated reads of (e.g. single-end reads)")
    ("mates1,1", po::value<vector<string>>(&mate1ReadFiles)->multitoken(),
        "File containing the #1 mates")
    ("mates2,2", po::value<vector<string>>(&mate2ReadFiles)->multitoken(),
        "File containing the #2 mates")
    ("threads,p", po::value<uint32_t>()->default_value(maxThreads), "The number of threads to use concurrently.")
    ("output,o", po::value<std::string>(), "Output quantification file");

    po::variables_map vm;
    try {
        auto orderedOptions = po::command_line_parser(argc,argv).
            options(generic).run();

        po::store(orderedOptions, vm);

        if ( vm.count("help") ) {
            auto hstring = R"(
Quant
==========
Perform streaming k-mer-group-based estimation of
transcript abundance from RNA-seq reads
)";
            std::cout << hstring << std::endl;
            std::cout << generic << std::endl;
            std::exit(1);
        }

        po::notify(vm);


        for (auto& opt : orderedOptions.options) {
            std::cerr << "[ " << opt.string_key << "] => {";
            for (auto& val : opt.value) {
                std::cerr << " " << val;
            }
            std::cerr << " }\n";
        }

        bfs::path outputDirectory(vm["output"].as<std::string>());
        bfs::create_directory(outputDirectory);
        if (!(bfs::exists(outputDirectory) and bfs::is_directory(outputDirectory))) {
            std::cerr << "Couldn't create output directory " << outputDirectory << "\n";
            std::cerr << "exiting\n";
            std::exit(1);
        }

        bfs::path indexDirectory(vm["index"].as<string>());
        bfs::path logDirectory = outputDirectory.parent_path() / "logs";

#if HAVE_LOGGER
        bfs::create_directory(logDirectory);
        if (!(bfs::exists(logDirectory) and bfs::is_directory(logDirectory))) {
            std::cerr << "Couldn't create log directory " << logDirectory << "\n";
            std::cerr << "exiting\n";
            std::exit(1);
        }
        std::cerr << "writing logs to " << logDirectory.string() << "\n";
        g2LogWorker logger(argv[0], logDirectory.string());
        g2::initializeLogging(&logger);
#endif

        vector<ReadLibrary> readLibraries;
        for (auto& opt : orderedOptions.options) {
            if (opt.string_key == "libtype") {
                LibraryFormat libFmt = sailfish::utils::parseLibraryFormatString(opt.value[0]);
                if (libFmt.check()) {
                    std::cerr << libFmt << "\n";
                } else {
                    std::stringstream ss;
                    ss << libFmt << " is invalid!";
                    throw std::invalid_argument(ss.str());
                }
                readLibraries.emplace_back(libFmt);
            } else if (opt.string_key == "mates1") {
                readLibraries.back().addMates1(opt.value);
            } else if (opt.string_key == "mates2") {
                readLibraries.back().addMates2(opt.value);
            } else if (opt.string_key == "unmated_reads") {
                readLibraries.back().addUnmated(opt.value);
            }
        }

        for (auto& rl : readLibraries) { rl.checkValid(); }

        auto& rl = readLibraries.front();
        // Handle all the proper cases here
        char* readFiles[] = { const_cast<char*>(rl.mates1().front().c_str()),
                              const_cast<char*>(rl.mates2().front().c_str()) };

        uint32_t nbThreads = vm["threads"].as<uint32_t>();

        size_t maxReadGroup{2000}; // Number of files to read simultaneously
        size_t concurrentFile{1}; // Number of reads in each "job"
        sequence_parser parser(4 * nbThreads, maxReadGroup, concurrentFile,
                readFiles, readFiles + 2);

        bfs::path sfIndexPath = indexDirectory / "transcriptome.sfi";
        std::string sfTrascriptIndexFile(sfIndexPath.string());
        std::cerr << "reading index . . . ";
        auto phi = PerfectHashIndex::fromFile(sfTrascriptIndexFile);
        std::cerr << "done\n";
        std::cerr << "index contained " << phi.numKeys() << " kmers\n";

        size_t nkeys = phi.numKeys();
        size_t merLen = phi.kmerLength();


        google::dense_hash_map<uint64_t, uint64_t> khash;
        /*
        khash.set_empty_key(std::numeric_limits<uint64_t>::max());
        uint64_t i{0};
        for (auto k : phi.kmers()) {
            khash[k] = phi.index(k);
        }
        */

        std::cerr << "kmer length = " << merLen << "\n";
        my_mer::k(merLen);

        bfs::path tlutPath = indexDirectory / "transcriptome.tlut";
        // Get transcript lengths
        std::ifstream ifile(tlutPath.string(), std::ios::binary);
        size_t numRecords {0};
        ifile.read(reinterpret_cast<char *>(&numRecords), sizeof(numRecords));

        std::vector<Transcript> transcripts_tmp;

        std::cerr << "Transcript LUT contained " << numRecords << " records\n";
        //transcripts_.resize(numRecords);
        for (auto i : boost::irange(size_t(0), numRecords)) {
            auto ti = LUTTools::readTranscriptInfo(ifile);
            // copy over the length, then we're done.
            transcripts_tmp.emplace_back(ti->transcriptID, ti->name.c_str(), ti->length);
        }

        std::sort(transcripts_tmp.begin(), transcripts_tmp.end(),
                [](const Transcript& t1, const Transcript& t2) -> bool {
                return t1.id < t2.id;
                });
        std::vector<Transcript> transcripts_;
        for (auto& t : transcripts_tmp) {
            transcripts_.emplace_back(t.id, t.RefName.c_str(), t.RefLength);
        }
        transcripts_tmp.clear();
        ifile.close();
        // --- done ---

        bfs::path locPath = indexDirectory / "fullLookup.kmap";
        // Make sure we have the file we need to look up k-mer hits
        if (!boost::filesystem::exists(locPath)) {
            std::cerr << "could not find k-mer location index; expected at " << locPath << "\n";
            std::cerr << "please ensure that you've run salmon index before attempting to run salmon quant\n";
            std::exit(1);
        }
        std::cerr << "Loading k-mer location index from " << locPath << " . . .";

        //std::vector<std::vector<uint64_t>> kmerLocMap;
        std::vector<uint64_t> offsets;
        std::vector<uint64_t> kmerLocs;

        std::ifstream binstream(locPath.string(), std::ios::binary);
        cereal::BinaryInputArchive archive(binstream);
        //archive(kmerLocMap);
        archive(offsets, kmerLocs);
        binstream.close();
        std::cerr << "done\n";

        std::vector<std::thread> threads;
        std::atomic<uint64_t> total_fwd(0);
        std::atomic<uint64_t> total_bwd(0);
        std::atomic<uint64_t> totalHits(0);
        std::atomic<uint64_t> rn{0};
        std::atomic<uint64_t> batchNum{0};

        size_t numTranscripts{transcripts_.size()};
        ClusterForest clusterForest(numTranscripts, transcripts_);

        double logForgettingMass{sailfish::math::LOG_1};
        std::mutex ffmutex;
        for(int i = 0; i < nbThreads; ++i)  {
            std::cerr << "here\n";
            threads.push_back(std::thread(add_sizes, &parser, &total_fwd, &total_bwd,
                        std::ref(totalHits), std::ref(rn),
                        std::ref(phi),
                        std::ref(transcripts_),
                        std::ref(batchNum),
                        std::ref(logForgettingMass),
                        std::ref(ffmutex),
                        std::ref(clusterForest),
                        std::ref(offsets),
                        std::ref(kmerLocs),
                        std::ref(khash)
                        //std::ref(kmerLocMap)
                        ));
        }

        for(int i = 0; i < nbThreads; ++i)
            threads[i].join();

        std::cerr << "\n\n";
        std::cerr << "processed " << rn << " total reads\n";
        std::cout << "Total bases: " << total_fwd << " " << total_bwd << "\n";
        std::cout << "Had a hit for " << totalHits  / static_cast<double>(rn) * 100.0 << "% of the reads\n";

        size_t tnum{0};

        std::cerr << "writing output \n";

        bfs::path outputFile = outputDirectory / "quant.sf";
        std::ofstream output(outputFile.string());
        output << "# SDAFish v0.01\n";
        output << "# ClusterID\tName\tLength\tFPKM\tNumReads\n";

        const double logBillion = std::log(1000000000.0);
        const double logNumFragments = std::log(static_cast<double>(rn));
        auto clusters = clusterForest.getClusters();
        size_t clusterID = 0;
        for(auto cptr : clusters) {
            double logClusterMass = cptr->logMass();
            double logClusterCount = std::log(static_cast<double>(cptr->numHits()));

            if (logClusterMass == sailfish::math::LOG_0) {
                std::cerr << "Warning: cluster " << clusterID << " has 0 mass!\n";
            }

            bool requiresProjection{false};

            auto& members = cptr->members();
            size_t clusterSize{0};
            for (auto transcriptID : members) {
                Transcript& t = transcripts_[transcriptID];
                t.uniqueCounts = t.uniqueCount();
                t.totalCounts = t.totalCount();
            }

            for (auto transcriptID : members) {
                Transcript& t = transcripts_[transcriptID];
                double logTranscriptMass = t.mass();
                double logClusterFraction = logTranscriptMass - logClusterMass;
                t.projectedCounts = std::exp(logClusterFraction + logClusterCount);
                requiresProjection |= t.projectedCounts > static_cast<double>(t.totalCounts) or
                    t.projectedCounts < static_cast<double>(t.uniqueCounts);
                ++clusterSize;
            }

            if (clusterSize > 1 and requiresProjection) {
                cptr->projectToPolytope(transcripts_);
            }

            // Now posterior has the transcript fraction
            size_t idx = 0;
            for (auto transcriptID : members) {
                auto& transcript = transcripts_[transcriptID];
                double logLength = std::log(transcript.RefLength);
                double fpkmFactor = std::exp(logBillion - logLength - logNumFragments);
                double count = transcripts_[transcriptID].projectedCounts;
                double countTotal = transcripts_[transcriptID].totalCounts;
                double countUnique = transcripts_[transcriptID].uniqueCounts;
                double fpkm = count > 0 ? fpkmFactor * count : 0.0;
                output << clusterID << '\t' << transcript.RefName << '\t' <<
                    transcript.RefLength << '\t' << fpkm << '\t' << countTotal << '\t' << countUnique << '\t' << count <<  '\t' << transcript.mass() << '\n';
                ++idx;
            }

            ++clusterID;
        }
        output.close();

    } catch (po::error &e) {
        std::cerr << "Exception : [" << e.what() << "]. Exiting.\n";
        std::exit(1);
    } catch (std::exception& e) {
        std::cerr << "Exception : [" << e.what() << "]\n";
        std::cerr << argv[0] << " quant was invoked improperly.\n";
        std::cerr << "For usage information, try " << argv[0] << " quant --help\nExiting.\n";
    }


    return 0;
}


