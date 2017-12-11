#include "engine/effects/engineeffectsmanager.h"

#include "engine/effects/engineeffectrack.h"
#include "engine/effects/engineeffectchain.h"
#include "engine/effects/engineeffect.h"

#include "util/defs.h"
#include "util/sample.h"

EngineEffectsManager::EngineEffectsManager(EffectsResponsePipe* pResponsePipe)
        : m_pResponsePipe(pResponsePipe),
          m_buffer1(MAX_BUFFER_LEN),
          m_buffer2(MAX_BUFFER_LEN) {
    // Try to prevent memory allocation.
    m_preFaderRacks.reserve(256);
    m_postFaderRacks.reserve(256);
    m_chains.reserve(256);
    m_effects.reserve(256);
}

EngineEffectsManager::~EngineEffectsManager() {
}

void EngineEffectsManager::onCallbackStart() {
    EffectsRequest* request = NULL;
    while (m_pResponsePipe->readMessages(&request, 1) > 0) {
        EffectsResponse response(*request);
        bool processed = false;
        switch (request->type) {
            case EffectsRequest::ADD_EFFECT_RACK:
            case EffectsRequest::REMOVE_EFFECT_RACK:
                if (processEffectsRequest(*request, m_pResponsePipe.data())) {
                    processed = true;
                }
                break;
            case EffectsRequest::ADD_CHAIN_TO_RACK:
            case EffectsRequest::REMOVE_CHAIN_FROM_RACK:
                VERIFY_OR_DEBUG_ASSERT(m_preFaderRacks.contains(request->pTargetRack)
                        || m_postFaderRacks.contains(request->pTargetRack)) {
                    response.success = false;
                    response.status = EffectsResponse::NO_SUCH_RACK;
                    break;
                }

                processed = request->pTargetRack->processEffectsRequest(
                    *request, m_pResponsePipe.data());

                if (processed) {
                    // When an effect-chain becomes active (part of a rack), keep
                    // it in our master list so that we can respond to
                    // requests about it.
                    if (request->type == EffectsRequest::ADD_CHAIN_TO_RACK) {
                        m_chains.append(request->AddChainToRack.pChain);
                    } else if (request->type == EffectsRequest::REMOVE_CHAIN_FROM_RACK) {
                        m_chains.removeAll(request->RemoveChainFromRack.pChain);
                    }
                } else {
                    if (!processed) {
                        // If we got here, the message was not handled for
                        // an unknown reason.
                        response.success = false;
                        response.status = EffectsResponse::INVALID_REQUEST;
                    }
                }
                break;
            case EffectsRequest::ADD_EFFECT_TO_CHAIN:
            case EffectsRequest::REMOVE_EFFECT_FROM_CHAIN:
            case EffectsRequest::SET_EFFECT_CHAIN_PARAMETERS:
            case EffectsRequest::ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL:
            case EffectsRequest::DISABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL:
                VERIFY_OR_DEBUG_ASSERT(m_chains.contains(request->pTargetChain)) {
                    response.success = false;
                    response.status = EffectsResponse::NO_SUCH_CHAIN;
                    break;
                }

                processed = request->pTargetChain->processEffectsRequest(
                    *request, m_pResponsePipe.data());
                if (processed) {
                    // When an effect becomes active (part of a chain), keep
                    // it in our master list so that we can respond to
                    // requests about it.
                    if (request->type == EffectsRequest::ADD_EFFECT_TO_CHAIN) {
                        m_effects.append(request->AddEffectToChain.pEffect);
                    } else if (request->type == EffectsRequest::REMOVE_EFFECT_FROM_CHAIN) {
                        m_effects.removeAll(request->RemoveEffectFromChain.pEffect);
                    }
                } else {
                    // If we got here, the message was not handled for
                    // an unknown reason.
                    response.success = false;
                    response.status = EffectsResponse::INVALID_REQUEST;
                }
                break;
            case EffectsRequest::SET_EFFECT_PARAMETERS:
            case EffectsRequest::SET_PARAMETER_PARAMETERS:
                VERIFY_OR_DEBUG_ASSERT(m_effects.contains(request->pTargetEffect)) {
                    response.success = false;
                    response.status = EffectsResponse::NO_SUCH_EFFECT;
                    break;
                }

                processed = request->pTargetEffect
                        ->processEffectsRequest(*request, m_pResponsePipe.data());

                if (!processed) {
                    // If we got here, the message was not handled for an
                    // unknown reason.
                    response.success = false;
                    response.status = EffectsResponse::INVALID_REQUEST;
                }
                break;
            default:
                response.success = false;
                response.status = EffectsResponse::UNHANDLED_MESSAGE_TYPE;
                break;
        }

        if (!processed) {
            m_pResponsePipe->writeMessages(&response, 1);
        }
    }
}

void EngineEffectsManager::processPreFaderInPlace(const ChannelHandle& inputHandle,
                                                  const ChannelHandle& outputHandle,
                                                  CSAMPLE* pInOut,
                                                  const unsigned int numSamples,
                                                  const unsigned int sampleRate) {
    // Feature state is gathered after prefader effects processing.
    // This is okay because the equalizer and filter effects do not make use of it.
    // However, if an effect is loaded into a QuickEffectRack that could make use
    // of the GroupFeatureState, it will not sound the same as if it is loaded into
    // a StandardEffectRack.
    GroupFeatureState featureState;
    processInner(m_preFaderRacks,
                 inputHandle, outputHandle,
                 pInOut, pInOut,
                 numSamples, sampleRate, featureState);
}

void EngineEffectsManager::processPostFaderInPlace(
    const ChannelHandle& inputHandle,
    const ChannelHandle& outputHandle,
    CSAMPLE* pInOut,
    const unsigned int numSamples,
    const unsigned int sampleRate,
    const GroupFeatureState& groupFeatures,
    const CSAMPLE_GAIN oldGain,
    const CSAMPLE_GAIN newGain) {
    processInner(m_postFaderRacks,
                 inputHandle, outputHandle,
                 pInOut, pInOut,
                 numSamples, sampleRate, groupFeatures,
                 oldGain, newGain);
}

void EngineEffectsManager::processPostFaderAndMix(
    const ChannelHandle& inputHandle,
    const ChannelHandle& outputHandle,
    CSAMPLE* pIn, CSAMPLE* pOut,
    const unsigned int numSamples,
    const unsigned int sampleRate,
    const GroupFeatureState& groupFeatures,
    const CSAMPLE_GAIN oldGain,
    const CSAMPLE_GAIN newGain) {
    processInner(m_postFaderRacks,
                 inputHandle, outputHandle,
                 pIn, pOut,
                 numSamples, sampleRate, groupFeatures,
                 oldGain, newGain);
}

void EngineEffectsManager::processInner(
    const QList<EngineEffectRack*>& racks,
    const ChannelHandle& inputHandle,
    const ChannelHandle& outputHandle,
    CSAMPLE* pIn, CSAMPLE* pOut,
    const unsigned int numSamples,
    const unsigned int sampleRate,
    const GroupFeatureState& groupFeatures,
    const CSAMPLE_GAIN oldGain,
    const CSAMPLE_GAIN newGain) {
    if (pIn == pOut) {
        // Gain and effects are applied to the buffer in place,
        // modifying the original input buffer
        SampleUtil::applyRampingGain(pIn, oldGain, newGain, numSamples);
        for (EngineEffectRack* pRack : racks) {
            if (pRack != nullptr) {
                pRack->process(inputHandle, outputHandle,
                               pIn, pIn,
                               numSamples, sampleRate, groupFeatures);
            }
        }
    } else {
        // Do not modify the input buffer.
        // 1. Copy input buffer to a temporary buffer
        // 2. Apply gain to temporary buffer
        // 2. Process temporary buffer with each effect rack in series
        // 3. Mix the temporary buffer into pOut
        //    ChannelMixer::applyEffectsAndMixChannels use
        //    this to mix channels into pOut regardless of whether any effects were processsed.
        CSAMPLE* pIntermediateInput = m_buffer1.data();
        if (oldGain == CSAMPLE_GAIN_ONE && newGain == CSAMPLE_GAIN_ONE) {
            // Avoid an unnecessary copy. EngineEffectRack::process does not modify the
            // input buffer when its input & output buffers are different, so this is okay.
            pIntermediateInput = pIn;
        } else {
            SampleUtil::copyWithRampingGain(pIntermediateInput, pIn,
                                            oldGain, newGain, numSamples);
        }

        CSAMPLE* pIntermediateOutput;
        for (EngineEffectRack* pRack : racks) {
            if (pRack != nullptr) {
                // Select an unused intermediate buffer for the next output
                if (pIntermediateInput == m_buffer1.data()) {
                    pIntermediateOutput = m_buffer2.data();
                } else {
                    pIntermediateOutput = m_buffer1.data();
                }

                if (pRack->process(inputHandle, outputHandle,
                                   pIntermediateInput, pIntermediateOutput,
                                   numSamples, sampleRate, groupFeatures)) {
                    // Output of this rack becomes the input of the next rack.
                    pIntermediateInput = pIntermediateOutput;
                }
            }
        }
        // pIntermediateInput is the output of the last processed rack. It would be the
        // intermediate input of the next rack if there was one.
        SampleUtil::add(pOut, pIntermediateInput, numSamples);
    }
}

bool EngineEffectsManager::addPostFaderEffectRack(EngineEffectRack* pRack) {
    VERIFY_OR_DEBUG_ASSERT(!m_postFaderRacks.contains(pRack)) {
        return false;
    }
    m_postFaderRacks.append(pRack);
    return true;
}

bool EngineEffectsManager::removePostFaderEffectRack(EngineEffectRack* pRack) {
    return m_postFaderRacks.removeAll(pRack) > 0;
}

bool EngineEffectsManager::addPreFaderEffectRack(EngineEffectRack* pRack) {
    VERIFY_OR_DEBUG_ASSERT(!m_preFaderRacks.contains(pRack)) {
        return false;
    }
    m_preFaderRacks.append(pRack);
    return true;
}

bool EngineEffectsManager::removePreFaderEffectRack(EngineEffectRack* pRack) {
    return m_preFaderRacks.removeAll(pRack) > 0;
}

bool EngineEffectsManager::processEffectsRequest(const EffectsRequest& message,
                                                 EffectsResponsePipe* pResponsePipe) {
    EffectsResponse response(message);
    switch (message.type) {
        case EffectsRequest::ADD_EFFECT_RACK:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << "ADD_EFFECT_RACK"
                         << message.AddEffectRack.pRack
                         << message.AddEffectRack.preFader;
            }
            if (message.AddEffectRack.preFader) {
                response.success = addPreFaderEffectRack(message.AddEffectRack.pRack);
            } else {
                response.success = addPostFaderEffectRack(message.AddEffectRack.pRack);
            }
            break;
        case EffectsRequest::REMOVE_EFFECT_RACK:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << "REMOVE_EFFECT_RACK"
                         << message.RemoveEffectRack.pRack
                         << message.RemoveEffectRack.preFader;
            }
            if (message.AddEffectRack.preFader) {
                response.success = removePreFaderEffectRack(message.AddEffectRack.pRack);
            } else {
                response.success = removePostFaderEffectRack(message.AddEffectRack.pRack);
            }
            break;
        default:
            return false;
    }
    pResponsePipe->writeMessages(&response, 1);
    return true;
}
