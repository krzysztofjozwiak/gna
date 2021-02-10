/**
 @copyright (C) 2019-2021 Intel Corporation
 SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "AffineFunctions.h"

#include "AccelerationDetector.h"
#include "ActiveList.h"
#include "AffineLayerCapabilities.h"
#include "Bias.h"
#include "Capabilities.h"
#include "DataMode.h"
#include "Expect.h"
#include "GnaException.h"
#include "LayerConfiguration.h"
#include "OperationConfig.h"
#include "Shape.h"
#include "Validator.h"
#include "Weight.h"

#include "gna2-common-api.h"

#include "common.h"
#include "gna-api.h"
#include "gna-api-types-xnn.h"

#include <algorithm>
#include <cstdint>
#include <memory>

using namespace GNA;

const FullCapabilitiesMap AffineFunctionSingle::outputCapabilities =
{
    {INTEL_AFFINE, {
        AffineLayerCapabilities::GetOperands(OutputOperandIndex).at(INTEL_AFFINE)
    }},
    {INTEL_AFFINE_DIAGONAL, {
        AffineLayerCapabilities::GetOperands(OutputOperandIndex).at(INTEL_AFFINE_DIAGONAL)
    }},
    {INTEL_RECURRENT, {
        AffineLayerCapabilities::GetOperands(OutputOperandIndex).at(INTEL_RECURRENT)
    }}
};

const FullCapabilitiesMap AffineFunctionMulti::outputCapabilities =
{
    {INTEL_AFFINE_MULTIBIAS, {
        AffineLayerCapabilities::GetOperands(OutputOperandIndex).at(INTEL_AFFINE_MULTIBIAS)
    }},
};

const FullCapabilitiesMap AffineFunctionMulti::Capabilities =
{
    {INTEL_AFFINE_MULTIBIAS, {
        AffineLayerCapabilities::GetOperands(WeightScaleFactorOperandIndex).at(INTEL_AFFINE_MULTIBIAS)
    }},
};

// Could not split into separate methods for each component as multibias weight scaling is using bias' and weights; tensors...
std::unique_ptr<AffineFunction> AffineFunction::Create(
        const TransformFactoryConfig& config,
        const OperationConfig& operationConfig)
{
    if (operationConfig.BiasMode == Gna2BiasModeGrouping)
    {
        return createAffineMultiFunction(config, operationConfig);
    }

    return createAffineSingleFunction(config, operationConfig);
}

std::unique_ptr<AffineFunction> AffineFunction::createAffineSingleFunction(
    const TransformFactoryConfig& config, const OperationConfig& operationConfig)
{
    auto kernelOperation = operationConfig.GetKernelOperation();
    auto weightTensor = operationConfig.WeightsTensor;
    auto biasTensor = operationConfig.BiasesTensor;
    auto weights = std::make_unique<const WeightTensor>(weightTensor, config.validator);
    auto biases = std::make_unique<const BiasTensor>(
            biasTensor, 0, Gna2BiasModeDefault, config.validator);
    auto kernelMode = KernelMode { config.input->Mode, weights->Mode, biases->Mode };
    const auto& affineKernel = AccelerationDetector::GetKernelMap<AffineKernel>(
            static_cast<kernel_op>(kernelOperation), kernelMode);
    return std::make_unique<AffineFunctionSingle>(
            BaseTransformConfig<AffineKernel>{config, affineKernel},
            operationConfig.GetTransformOperation(),
            std::move(weights), std::move(biases));
}

std::unique_ptr<AffineFunction> AffineFunction::createAffineMultiFunction(
    const TransformFactoryConfig& config, const OperationConfig& operationConfig)
{
    std::unique_ptr<const Tensor> weightScales;
    auto weightTensor = operationConfig.WeightsTensor;
    auto biasTensor = operationConfig.BiasesTensor;
    auto biasVectorIndex = operationConfig.BiasVectorIndex;
    auto weights = std::make_unique<const WeightTensor>(weightTensor, config.validator);
    auto biases = std::make_unique<const BiasTensor>(biasTensor, biasVectorIndex,
            Gna2BiasModeGrouping, config.validator);

    if (operationConfig.WeightScalesTensor.Mode != Gna2TensorModeDisabled)
    {
        const std::function<void()> command = [&]()
        {
            weightScales = std::make_unique<const Tensor>(operationConfig.WeightScalesTensor,
                Validator{ config.validator, AffineFunctionMulti::Capabilities });
            ModelErrorHelper::ExpectNotNull(*weightScales);
        };
        ModelErrorHelper::ExecuteForModelItem(command, WeightScaleFactorOperandIndex);
    }

    auto kernelOperation = KERNEL_AFFINE_MULTIBIAS;
    auto kernelMode = KernelMode { config.input->Mode, weights->Mode, biases->Mode };
    auto& affineKernel = AccelerationDetector::GetKernelMap<AffineKernel>(
            static_cast<kernel_op>(kernelOperation), kernelMode);
    return std::make_unique<AffineFunctionMulti>(BaseTransformConfig<AffineKernel>{config, affineKernel},
            operationConfig.GetTransformOperation(),
            std::move(weights), std::move(biases),
            std::move(weightScales));
}

AffineFunction::AffineFunction(const BaseTransformConfig<AffineKernel>& config,
    TransformOperation transform,
    std::unique_ptr<const WeightTensor> weights,
    std::unique_ptr<const BiasTensor> biases) :
    Transform{transform, &config.kernels, config.input},
    Weights{ std::move(weights) },
    Biases{ std::move(biases) }
{
}

Tensor const& AffineFunction::GetOperand(uint32_t operandIndex) const
{
    switch (operandIndex)
    {
    case WeightOperandIndex:
    {
        return GetOperandIfExistOrThrow(Weights);
    }
    case BiasOperandIndex:
    {
        return GetOperandIfExistOrThrow(Biases);
    }
    default:
        return Transform::GetOperand(operandIndex);
    }
}

AffineFunctionSingle::AffineFunctionSingle(
    BaseTransformConfig<AffineKernel> config, TransformOperation transform,
    std::unique_ptr<const WeightTensor> weights, std::unique_ptr<const BiasTensor> biases)
    : AffineFunction(config, transform, std::move(weights), std::move(biases)),
    kernelsAl(AccelerationDetector::GetKernelMap<AffineActiveListKernel>(
        KERNEL_AFFINE_AL, KernelMode { config.input->Mode, Weights->Mode, Biases->Mode }))
{
    //Expect::True(GNA_INT32 == Biases->Mode, Gna2StatusXnnErrorBiasBytes);
    //Expect::True(GNA_DATA_RICH_FORMAT == Biases->Mode, Gna2StatusXnnErrorBiasBytes);
    AffineConfig kernelAffineConfig = { config.output->Dimensions.at('H'),
        config.output->Dimensions.at('W'), config.input->Dimensions.at('H'),
        config.input->Buffer, config.output->Buffer, *Weights,
        *Biases, nullptr, 0, Biases->Mode.Size};

    Output = std::make_unique<Tensor>(
        Shape{GNA_TENSOR_HW, config.output->Dimensions.at('H'), config.output->Dimensions.at('W')},
        config.output->Mode, config.outputBuffer,
        Validator{config.validator, outputCapabilities});

    hiddenConfig = std::make_unique<KernelConfig<AffineConfig>>(kernelAffineConfig,
            BaseConfig { Input->Buffer, Output->Buffer });
}

void AffineFunctionSingle::ValidateActiveList(ActiveList const& activeList) const
{
    Expect::InRange(activeList.IndicesCount,
        ui32_1, Output->at(GNA_DIM_H), Gna2StatusActiveListIndicesInvalid);
    // Only Int32 is supported with active list
    Expect::InSet(Biases->Mode.Type,
        { Gna2DataTypeInt32, Gna2DataTypeCompoundBias },
        Gna2StatusModelConfigurationInvalid);
}

void AffineFunctionSingle::Compute(AccelerationMode accel,
    LayerConfiguration const * layerConfiguration, ExecutionConfig const & execution) const
{
    auto executionConfig = createExecutionConfig(layerConfiguration, execution);
    try
    {
        if (layerConfiguration != nullptr && layerConfiguration->ActList)
        {
            kernelsAl.at(accel)(executionConfig.get(), AffineConfigAl{
                                layerConfiguration->ActList->Indices,
                                layerConfiguration->ActList->IndicesCount});
        }
        else
        {
            kernels->at(accel)(executionConfig.get());
        }
    }
    catch (const std::out_of_range&)
    {
        throw GnaException(Gna2StatusNotImplemented);
    }
}

AffineFunctionMulti::AffineFunctionMulti(BaseTransformConfig<AffineKernel> config,
    TransformOperation transform,
    std::unique_ptr<const WeightTensor> weights, std::unique_ptr<const BiasTensor> biases,
    std::unique_ptr<const Tensor> weightScaleFactors) :
    AffineFunction(config, transform, std::move(weights), std::move(biases)),
    WeightScaleFactors{ std::move(weightScaleFactors) }
{
    AffineConfig kernelAffineConfig = { config.output->Dimensions.at('H'),
        config.input->Dimensions.at('W'), config.input->Dimensions.at('H'),
        config.input->Buffer, config.output->Buffer, *Weights,
        (WeightScaleFactors ? static_cast<const void*>(*WeightScaleFactors) : nullptr),
        *Biases, Biases->Dimensions.at('W'), Biases->Mode.Size };

    Output = std::make_unique<Tensor>(
        Shape{GNA_TENSOR_HW, config.output->Dimensions.at('H'), config.output->Dimensions.at('W')},
        config.output->Mode, config.outputBuffer,
        Validator{config.validator, outputCapabilities});

    hiddenConfig = std::make_unique<KernelConfig<AffineConfig>>(kernelAffineConfig,
            BaseConfig { Input->Buffer, Output->Buffer });
}

Tensor const& AffineFunctionMulti::GetOperand(uint32_t operandIndex) const
{
    switch (operandIndex)
    {
    case WeightScaleFactorOperandIndex:
    {
        return GetOperandIfExistOrThrow(WeightScaleFactors);
    }
    default:
        return AffineFunction::GetOperand(operandIndex);
    }
}
