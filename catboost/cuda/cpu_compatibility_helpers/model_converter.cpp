#include "model_converter.h"

#include <catboost/cuda/data/binarizations_manager.h>
#include <catboost/cuda/data/data_provider.h>
#include <catboost/cuda/models/oblivious_model.h>
#include <catboost/cuda/models/additive_model.h>
#include <catboost/cuda/data/cat_feature_perfect_hash.h>
#include <catboost/libs/model/model.h>
#include <catboost/libs/model/target_classifier.h>
#include <catboost/libs/algo/projection.h>
#include <catboost/libs/algo/split.h>
#include <catboost/libs/model/model_build_helper.h>
#include <limits>

TVector<TVector<int>> NCatboostCuda::MakeInverseCatFeatureIndexForDataProviderIds(
        const NCatboostCuda::TBinarizedFeaturesManager& featuresManager,
        const TVector<ui32>& catFeaturesDataProviderIds, bool clearFeatureManagerRamCache) {
    TVector<TVector<int>> result(catFeaturesDataProviderIds.size());
    for (ui32 i = 0; i < catFeaturesDataProviderIds.size(); ++i) {
        const ui32 featureManagerId = featuresManager.GetFeatureManagerIdForCatFeature(
                catFeaturesDataProviderIds[i]);
        const auto& perfectHash = featuresManager.GetCategoricalFeaturesPerfectHash(featureManagerId);

        if (!perfectHash.empty()) {
            result[i].resize(perfectHash.size());
            for (const auto& entry : perfectHash) {
                result[i][entry.second] = entry.first;
            }
        }
    }
    if (clearFeatureManagerRamCache) {
        featuresManager.UnloadCatFeaturePerfectHashFromRam();
    }
    return result;
}

TVector<TTargetClassifier> NCatboostCuda::CreateTargetClassifiers(const NCatboostCuda::TBinarizedFeaturesManager& featuresManager)  {
    TTargetClassifier targetClassifier(featuresManager.GetTargetBorders());
    TVector<TTargetClassifier> classifiers;
    classifiers.resize(1, targetClassifier);
    return classifiers;
}

namespace NCatboostCuda {

    TModelConverter::TModelConverter(const TBinarizedFeaturesManager& manager,
                                     const TDataProvider& dataProvider)
            : FeaturesManager(manager)
            , DataProvider(dataProvider)
    {
        auto& allFeatures = dataProvider.GetFeatureNames();
        auto& catFeatureIds = dataProvider.GetCatFeatureIds();

        {
            for (ui32 featureId = 0; featureId < allFeatures.size(); ++featureId) {
                if (catFeatureIds.has(featureId)) {
                    CatFeaturesRemap[featureId] = static_cast<ui32>(CatFeaturesRemap.size());
                } else {
                    if (dataProvider.HasFeatureId(featureId)) {
                        const auto featureIdInFeaturesManager = manager.GetFeatureManagerIdForFloatFeature(featureId);
                        TVector<float> borders = manager.GetBorders(featureIdInFeaturesManager);
                        Borders.push_back(std::move(borders));
                        FloatFeaturesNanMode.push_back(manager.GetNanMode(featureIdInFeaturesManager));
                    } else {
                        Borders.push_back(TVector<float>());
                        FloatFeaturesNanMode.push_back(ENanMode::Forbidden);
                    }
                    FloatFeaturesRemap[featureId] = static_cast<ui32>(FloatFeaturesRemap.size());
                }
            }
        }
        {
            TVector<ui32> catFeatureVec(catFeatureIds.begin(), catFeatureIds.end());
            CatFeatureBinToHashIndex = MakeInverseCatFeatureIndexForDataProviderIds(manager,
                                                                                    catFeatureVec);
        }
    }

    TFullModel TModelConverter::Convert(const TAdditiveModel<TObliviousTreeModel>& src) const {
        const auto& featureNames = DataProvider.GetFeatureNames();
        const auto& catFeatureIds = DataProvider.GetCatFeatureIds();
        TFullModel coreModel;
        coreModel.ModelInfo["params"] = "{}"; //will be overriden with correct params later

        ui32 cpuApproxDim = 1;

        if (DataProvider.IsMulticlassificationPool()) {
            coreModel.ModelInfo["multiclass_params"] = DataProvider.GetTargetHelper().Serialize();
            cpuApproxDim = DataProvider.GetTargetHelper().GetNumClasses();
        }

        auto featureCount = featureNames.ysize();
        TVector<TFloatFeature> floatFeatures;
        TVector<TCatFeature> catFeatures;

        for (int i = 0; i < featureCount; ++i) {
            if (catFeatureIds.has(i)) {
                auto catFeatureIdx = catFeatures.size();
                auto& catFeature = catFeatures.emplace_back();
                catFeature.FeatureIndex = catFeatureIdx;
                catFeature.FlatFeatureIndex = i;
                catFeature.FeatureId = featureNames[catFeature.FlatFeatureIndex];
                Y_ASSERT((ui32)catFeature.FeatureIndex == CatFeaturesRemap.at(i));
            } else {
                auto floatFeatureIdx = floatFeatures.size();
                auto& floatFeature = floatFeatures.emplace_back();
                const bool hasNans = FloatFeaturesNanMode.at(floatFeatureIdx) != ENanMode::Forbidden;
                floatFeature.FeatureIndex = floatFeatureIdx;
                floatFeature.FlatFeatureIndex = i;
                floatFeature.Borders = Borders[floatFeatureIdx];
                floatFeature.FeatureId = featureNames[i];
                floatFeature.HasNans = hasNans;
                if (hasNans) {
                    if (FloatFeaturesNanMode.at(floatFeatureIdx) == ENanMode::Min) {
                        floatFeature.NanValueTreatment = NCatBoostFbs::ENanValueTreatment_AsFalse;
                    } else {
                        floatFeature.NanValueTreatment = NCatBoostFbs::ENanValueTreatment_AsTrue;
                    }
                }
                Y_ASSERT((ui32)floatFeature.FeatureIndex == FloatFeaturesRemap.at(i));
            }
        }


        TObliviousTreeBuilder obliviousTreeBuilder(
                floatFeatures,
                catFeatures,
                cpuApproxDim);

        for (ui32 i = 0; i < src.Size(); ++i) {
            const TObliviousTreeModel& model = src.GetWeakModel(i);
            const ui32 outputDim = model.OutputDim();
            TVector<TVector<double>> leafValues(cpuApproxDim);
            TVector<double> leafWeights;

            const auto& values = model.GetValues();
            const auto& weights = model.GetWeights();

            leafWeights.resize(weights.size());
            for (ui32 leaf = 0; leaf < weights.size(); ++leaf) {
                leafWeights[leaf] = weights[leaf];
            }

            for (ui32 dim = 0; dim < cpuApproxDim; ++dim) {
                leafValues[dim].resize(model.BinCount());
                if (dim < outputDim) {
                    for (ui32 leaf = 0; leaf < model.BinCount(); ++leaf) {
                        const double val = values[outputDim * leaf + dim];
                        leafValues[dim][leaf] = val;
                    }
                }
            }


            const auto& structure = model.GetStructure();
            auto treeStructure = ConvertStructure(structure);
            obliviousTreeBuilder.AddTree(treeStructure, leafValues, leafWeights);
        }
        coreModel.ObliviousTrees = obliviousTreeBuilder.Build();
        return coreModel;
    }

    TModelSplit TModelConverter::CreateFloatSplit(const TBinarySplit& split) const {
        CB_ENSURE(FeaturesManager.IsFloat(split.FeatureId));

        TModelSplit modelSplit;
        modelSplit.Type = ESplitType::FloatFeature;
        auto dataProviderId = FeaturesManager.GetDataProviderId(split.FeatureId);
        CB_ENSURE(FloatFeaturesRemap.has(dataProviderId));
        auto remapId = FloatFeaturesRemap.at(dataProviderId);

        float border = 0;
        const auto nanMode = FloatFeaturesNanMode.at(remapId);
        switch (nanMode) {
            case ENanMode::Forbidden: {
                border = Borders.at(remapId).at(split.BinIdx);
                break;
            }
            case ENanMode::Min: {
                border = split.BinIdx != 0 ? Borders.at(remapId).at(split.BinIdx - 1) : std::numeric_limits<float>::lowest();
                break;
            }
            case ENanMode::Max: {
                border = split.BinIdx != Borders.at(remapId).size() ? Borders.at(remapId).at(split.BinIdx) : std::numeric_limits<float>::max();
                break;
            }
            default: {
                ythrow TCatboostException() << "Unknown NaN mode " << nanMode;
            };
        }
        modelSplit.FloatFeature = TFloatSplit{(int)remapId, border};
        return modelSplit;
    }

    TModelSplit TModelConverter::CreateOneHotSplit(const TBinarySplit& split) const {
        CB_ENSURE(FeaturesManager.IsCat(split.FeatureId));

        TModelSplit modelSplit;
        modelSplit.Type = ESplitType::OneHotFeature;
        auto dataProviderId = FeaturesManager.GetDataProviderId(split.FeatureId);
        CB_ENSURE(CatFeaturesRemap.has(dataProviderId));
        auto remapId = CatFeaturesRemap.at(dataProviderId);
        CB_ENSURE(CatFeatureBinToHashIndex[remapId].size(),
                  TStringBuilder() << "Error: no catFeature perfect hash for feature " << dataProviderId);
        CB_ENSURE(split.BinIdx < CatFeatureBinToHashIndex[remapId].size(),
                  TStringBuilder() << "Error: no hash for feature " << split.FeatureId << " " << split.BinIdx);
        const int hash = CatFeatureBinToHashIndex[remapId][split.BinIdx];
        modelSplit.OneHotFeature = TOneHotSplit(remapId,
                                                hash);
        return modelSplit;
    }

    ui32 TModelConverter::GetRemappedIndex(ui32 featureId) const {
        CB_ENSURE(FeaturesManager.IsCat(featureId) || FeaturesManager.IsFloat(featureId));
        ui32 dataProviderId = FeaturesManager.GetDataProviderId(featureId);
        if (FeaturesManager.IsFloat(featureId)) {
            return FloatFeaturesRemap.at(dataProviderId);
        } else {
            return CatFeaturesRemap.at(dataProviderId);
        }
    }

    TFeatureCombination TModelConverter::ExtractProjection(const TCtr& ctr) const  {
        TFeatureCombination projection;
        for (auto split : ctr.FeatureTensor.GetSplits()) {
            if (FeaturesManager.IsFloat(split.FeatureId)) {
                auto floatSplit = CreateFloatSplit(split);
                projection.BinFeatures.push_back(floatSplit.FloatFeature);
            } else if (FeaturesManager.IsCat(split.FeatureId)) {
                projection.OneHotFeatures.push_back(CreateOneHotSplit(split).OneHotFeature);
            } else {
                CB_ENSURE(false, "Error: unknown split type");
            }
        }
        for (auto catFeature : ctr.FeatureTensor.GetCatFeatures()) {
            projection.CatFeatures.push_back(GetRemappedIndex(catFeature));
        }
        //just for more more safety
        Sort(projection.BinFeatures.begin(), projection.BinFeatures.end());
        Sort(projection.CatFeatures.begin(), projection.CatFeatures.end());
        Sort(projection.OneHotFeatures.begin(), projection.OneHotFeatures.end());
        return projection;
    }

    TModelSplit TModelConverter::CreateCtrSplit(const TBinarySplit& split) const  {
        TModelSplit modelSplit;
        CB_ENSURE(FeaturesManager.IsCtr(split.FeatureId));
        const auto& ctr = FeaturesManager.GetCtr(split.FeatureId);
        auto& borders = FeaturesManager.GetBorders(split.FeatureId);
        CB_ENSURE(split.BinIdx < borders.size(), "Split " << split.BinIdx << ", borders: " << borders.size());

        modelSplit.Type = ESplitType::OnlineCtr;
        modelSplit.OnlineCtr.Border = borders[split.BinIdx];

        TModelCtr& modelCtr = modelSplit.OnlineCtr.Ctr;
        modelCtr.Base.Projection = ExtractProjection(ctr);
        modelCtr.Base.CtrType = ctr.Configuration.Type;
        modelCtr.Base.TargetBorderClassifierIdx = 0; // TODO(kirillovs): remove me

        const auto& config = ctr.Configuration;
        modelCtr.TargetBorderIdx = config.ParamId;
        modelCtr.PriorNum = GetNumeratorShift(config);
        modelCtr.PriorDenom = GetDenumeratorShift(config);

        return modelSplit;
    }

    TVector<TModelSplit> TModelConverter::ConvertStructure(const TObliviousTreeStructure& structure) const {
        TVector<TModelSplit> structure3;
        for (auto split : structure.Splits) {
            TModelSplit modelSplit;
            if (FeaturesManager.IsFloat(split.FeatureId)) {
                modelSplit = CreateFloatSplit(split);
            } else if (FeaturesManager.IsCat(split.FeatureId)) {
                modelSplit = CreateOneHotSplit(split);
            } else {
                modelSplit = CreateCtrSplit(split);
            }
            structure3.push_back(modelSplit);
        }
        return structure3;
    }
}
