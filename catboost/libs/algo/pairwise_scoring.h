#pragma once

#include "calc_score_cache.h"
#include "index_calcer.h"
#include "score_bin.h"
#include "split.h"

#include <catboost/libs/helpers/index_range.h>

#include <library/binsaver/bin_saver.h>

struct TBucketPairWeightStatistics {
    double SmallerBorderWeightSum = 0.0; // The weight sum of pair elements with smaller border.
    double GreaterBorderRightWeightSum = 0.0; // The weight sum of pair elements with greater border.

    void Add(const TBucketPairWeightStatistics& rhs) {
        SmallerBorderWeightSum += rhs.SmallerBorderWeightSum;
        GreaterBorderRightWeightSum += rhs.GreaterBorderRightWeightSum;
    }
    SAVELOAD(SmallerBorderWeightSum, GreaterBorderRightWeightSum);
};


struct TPairwiseStats {
    TVector<TVector<double>> DerSums; // [leafCount][bucketCount]
    TArray2D<TVector<TBucketPairWeightStatistics>> PairWeightStatistics; // [leafCount][leafCount][bucketCount]

    void Add(const TPairwiseStats& rhs);
    SAVELOAD(DerSums, PairWeightStatistics);
};


template<typename TBucketIndexType>
TVector<TVector<double>> ComputeDerSums(
    TConstArrayRef<double> weightedDerivativesData,
    int leafCount,
    int bucketCount,
    const TVector<TIndexType>& leafIndices,
    const TVector<TBucketIndexType>& bucketIndices,
    NCB::TIndexRange<int> docIndexRange
);

template<typename TBucketIndexType>
TArray2D<TVector<TBucketPairWeightStatistics>> ComputePairWeightStatistics(
    const TFlatPairsInfo& pairs,
    int leafCount,
    int bucketCount,
    const TVector<TIndexType>& leafIndices,
    const TVector<TBucketIndexType>& bucketIndices,
    NCB::TIndexRange<int> pairIndexRange
);

void CalculatePairwiseScore(
    const TPairwiseStats& pairwiseStats,
    int bucketCount,
    ESplitType splitType,
    float l2DiagReg,
    float pairwiseBucketWeightPriorReg,
    TVector<TScoreBin>* scoreBins
);

