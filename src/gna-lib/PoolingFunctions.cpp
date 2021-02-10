/**
 @copyright (C) 2018-2021 Intel Corporation
 SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "PoolingFunctions.h"

#include "AccelerationDetector.h"
#include "Expect.h"
#include "GnaException.h"
#include "ModelWrapper.h"
#include "PoolingMode.h"
#include "Validator.h"

#include <utility>

namespace GNA
{
struct PwlCached;
}

using namespace GNA;

const std::map<const nn_operation, const ShapeLimits> PoolingFunction::windowLimits =
{
    {INTEL_CONVOLUTIONAL,
        {{GNA_DIM_W, {CNN_POOL_SIZE_MIN, CNN_POOL_SIZE_MAX, 1, Gna2StatusCnnErrorPoolSize}}}
    },
};

const std::map<const nn_operation, const ShapeLimits> PoolingFunction::strideLimits =
{
    {INTEL_CONVOLUTIONAL,
        {{GNA_DIM_W, {CNN_POOL_SIZE_MIN, CNN_POOL_SIZE_MAX, 1, Gna2StatusCnnErrorPoolStride}}}
    },
};

std::unique_ptr<const PoolingFunction> PoolingFunction::Create(void const * layerDetails,
    const Shape & inputDimensions, const LayerValidator& validatorIn, gna_data_mode inputMode)
{
    Shape window;
    Shape stride;
    nn_pool_type type = INTEL_NO_POOLING;

    switch (validatorIn.Operation)
    {
    case INTEL_CONVOLUTIONAL:
    {
        auto cnn = static_cast<const nn_layer_conv*>(layerDetails);
        type = cnn->poolType;
        stride[GNA_DIM_W] = cnn->nPoolStride;
        window[GNA_DIM_W] = cnn->nPoolSize;

        break;
    }
    default:
        throw GnaException(Gna2StatusXnnErrorLyrOperation);
    }

    if (INTEL_NO_POOLING != type)
    {
        switch (validatorIn.Operation)
        {
        case INTEL_CONVOLUTIONAL:
            return std::make_unique<const PoolingFunction>(validatorIn.Operation, inputDimensions, window,
                stride, type,
                AccelerationDetector::GetKernelMap<ConvolutionPoolingKernel>(KERNEL_POOLING, inputMode));
        default:
            throw GnaException(Gna2StatusXnnErrorLyrOperation);
        }
    }
    return std::unique_ptr<const PoolingFunction>(nullptr);
}

void PoolingFunction::ExpectValid(Gna2Operation const & apiOperation)
{
    auto const hasPoolingWindow = ModelWrapper::HasParameter(apiOperation, PoolingWindowParamIndex);
    auto const hasPoolingStride = ModelWrapper::HasParameter(apiOperation, PoolingStrideParamIndex);

    if (hasPoolingWindow || hasPoolingStride)
    {
        ModelWrapper::ExpectParameterAvailable(apiOperation, PoolingModeParamIndex);
        ModelWrapper::ExpectParameterAvailable(apiOperation, PoolingWindowParamIndex);
        ModelWrapper::ExpectParameterAvailable(apiOperation, PoolingStrideParamIndex);
    }
}

std::unique_ptr<const PoolingFunction> PoolingFunction::Create(Gna2Operation const & apiOperation,
    const Shape & inputDimensions, const LayerValidator& validatorIn, gna_data_mode inputMode)
{
    Expect::Equal(INTEL_CONVOLUTIONAL, validatorIn.Operation, Gna2StatusXnnErrorLyrOperation);
    ExpectValid(apiOperation);

    const auto poolingMode = ModelWrapper::GetOptionalParameter<Gna2PoolingMode>(apiOperation, PoolingModeParamIndex,
        Gna2PoolingModeDisabled);

    if (Gna2PoolingModeMax == poolingMode || Gna2PoolingModeSum == poolingMode)
    {
        const auto apiStride = ModelWrapper::GetParameter<Gna2Shape>(
            apiOperation, PoolingStrideParamIndex);
        const auto strideShape = Shape::Create(apiStride, GNA_TENSOR_W);

        const auto apiWindow = ModelWrapper::GetParameter<Gna2Shape>(
            apiOperation, PoolingWindowParamIndex);
        const auto windowShape = Shape::Create(apiWindow, GNA_TENSOR_W);

        return std::make_unique<const PoolingFunction>(validatorIn.Operation, inputDimensions, windowShape,
            strideShape, poolingMode,
            AccelerationDetector::GetKernelMap<ConvolutionPoolingKernel>(KERNEL_POOLING, inputMode));
    }
    const std::function<void()> command = [&]()
    {
        ModelErrorHelper::ExpectInSet(poolingMode, { Gna2PoolingModeDisabled }, Gna2ItemTypeParameter);
    };
    ModelErrorHelper::ExecuteForModelItem(command, GNA2_DISABLED, PoolingModeParamIndex);
    return std::unique_ptr<const PoolingFunction>(nullptr);
}

PoolingFunction::PoolingFunction(nn_operation const operation, const Shape& inputDimensions,
    const Shape& window, const Shape& stride,
    const PoolingMode mode, const KernelMap<ConvolutionPoolingKernel>& kernelsIn) :
    Mode{ mode },
    Window{ window },
    Stride{ stride },
    kernels{ kernelsIn },
    hiddenConfig{ std::make_unique<PoolingConfig>(Mode, Window.at(GNA_DIM_W), Stride.at(GNA_DIM_W)) }
{
    Expect::InSet(Mode, { KernelPoolingModeMax, KernelPoolingModeSum }, Gna2StatusCnnErrorPoolType);
    const std::function<void()> poolingStrideValidation = [&]()
    {
        GNA::ExpectShapeIsValid(Stride, strideLimits.at(operation));
    };
    const std::function<void()> poolingWindowValidation = [&]()
    {
        GNA::ExpectShapeIsValid(Window, windowLimits.at(operation));
    };
    ModelErrorHelper::ExecuteForModelItem(poolingStrideValidation, GNA2_DISABLED, PoolingStrideParamIndex);
    ModelErrorHelper::ExecuteForModelItem(poolingWindowValidation, GNA2_DISABLED, PoolingWindowParamIndex);

    OutputsPerFilterCount = 1;
    OutputDimensions[GNA_DIM_D] = inputDimensions.at(GNA_DIM_D);
    for (const auto& dim : Stride)
    {
        if (GNA_DIM_D != dim.first)
        {
            OutputDimensions[dim.first] =  ((inputDimensions.at(dim.first) - 1) / dim.second + 1);
            OutputsPerFilterCount *= OutputDimensions[dim.first];
            Expect::InRange(OutputDimensions[dim.first], ui32_1, inputDimensions.at(dim.first), Gna2StatusCnnErrorPoolSize);
        }
    }
}

void PoolingFunction::Compute(const ConvolutionConfig * convolutionConfig, AccelerationMode accel, int64_t * poolScratchPad,
    const PwlCached * pwl) const
{
    auto poolConfig = PoolingConfig{ hiddenConfig.get(), poolScratchPad };
    kernels.at(accel)(convolutionConfig, &poolConfig, pwl);
}
