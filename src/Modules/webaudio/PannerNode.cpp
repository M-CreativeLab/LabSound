/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "LabSoundConfig.h"
#include "PannerNode.h"

#include "AudioBufferSourceNode.h"
#include "AudioBus.h"
#include "AudioContext.h"
#include "AudioNodeInput.h"
#include "AudioNodeOutput.h"
#include "HRTFPanner.h"
#include <wtf/MathExtras.h>

using namespace std;

namespace WebCore {

static void fixNANs(double &x)
{
    if (isnan(x) || isinf(x))
        x = 0.0;
}

PannerNode::PannerNode(std::shared_ptr<AudioContext> context, float sampleRate)
    : AudioNode(context, sampleRate)
    , m_panningModel(Panner::PanningModelHRTF)
    , m_lastGain(-1.0)
    , m_connectionCount(0)
{
    addInput(unique_ptr<AudioNodeInput>(new AudioNodeInput(this)));
    addOutput(unique_ptr<AudioNodeOutput>(new AudioNodeOutput(this, 2)));
    
    m_distanceGain = AudioGain::create(context, "distanceGain", 1.0, 0.0, 1.0);
    m_coneGain = AudioGain::create(context, "coneGain", 1.0, 0.0, 1.0);

    m_position = FloatPoint3D(0, 0, 0);
    m_orientation = FloatPoint3D(1, 0, 0);
    m_velocity = FloatPoint3D(0, 0, 0);
    
    setNodeType(NodeTypePanner);

    initialize();
}

PannerNode::~PannerNode()
{
    uninitialize();
}

void PannerNode::pullInputs(size_t framesToProcess)
{
    // We override pullInputs(), so we can detect new AudioSourceNodes which have connected to us when new connections are made.
    // These AudioSourceNodes need to be made aware of our existence in order to handle doppler shift pitch changes.

    ASSERT(!context().expired());
    auto ac = context().lock();
    
    if (m_connectionCount != ac->connectionCount()) {
        m_connectionCount = ac->connectionCount();

        // Recursively go through all nodes connected to us.
        notifyAudioSourcesConnectedToNode(this);
    }
    
    AudioNode::pullInputs(framesToProcess);
}

void PannerNode::process(size_t framesToProcess)
{
    AudioBus* destination = output(0)->bus();

    if (!isInitialized() || !input(0)->isConnected() || !m_panner.get()) {
        destination->zero();
        return;
    }

    AudioBus* source = input(0)->bus();

    if (!source) {
        destination->zero();
        return;
    }

    // Apply the panning effect.
    double azimuth;
    double elevation;
    getAzimuthElevation(&azimuth, &elevation);
    m_panner->pan(azimuth, elevation, source, destination, framesToProcess);

    // Get the distance and cone gain.
    double totalGain = distanceConeGain();

    // Snap to desired gain at the beginning.
    if (m_lastGain == -1.0)
        m_lastGain = totalGain;
        
    // Apply gain in-place with de-zippering.
    destination->copyWithGainFrom(*destination, &m_lastGain, totalGain);
}

void PannerNode::reset()
{
    m_lastGain = -1.0; // force to snap to initial gain
    if (m_panner.get())
        m_panner->reset();
}

void PannerNode::initialize()
{
    if (isInitialized())
        return;
        
    m_panner = Panner::create(m_panningModel, sampleRate());

    AudioNode::initialize();
}

void PannerNode::uninitialize()
{
    if (!isInitialized())
        return;
        
    m_panner.reset();
    AudioNode::uninitialize();
}

AudioListener* PannerNode::listener()
{
    ASSERT(!context().expired());
    return context().lock()->listener();
}

void PannerNode::setPanningModel(unsigned short model, ExceptionCode& ec)
{
    switch (model) {
    case EQUALPOWER:
    case HRTF:
        if (!m_panner.get() || model != m_panningModel) {
            m_panner = Panner::create(model, sampleRate());
            m_panningModel = model;
        }
        break;
    case SOUNDFIELD:
        // FIXME: Implement sound field model. See // https://bugs.webkit.org/show_bug.cgi?id=77367.
        // For now, fall through to throw an exception.
    default:
        ec = NOT_SUPPORTED_ERR;
        break;
    }
}

void PannerNode::setDistanceModel(unsigned short model, ExceptionCode& ec)
{
    switch (model) {
    case DistanceEffect::ModelLinear:
    case DistanceEffect::ModelInverse:
    case DistanceEffect::ModelExponential:
        m_distanceEffect.setModel(static_cast<DistanceEffect::ModelType>(model), true);
        break;
    default:
        ec = NOT_SUPPORTED_ERR;
        break;
    }
}

void PannerNode::getAzimuthElevation(double* outAzimuth, double* outElevation)
{
    // FIXME: we should cache azimuth and elevation (if possible), so we only re-calculate if a change has been made.

    double azimuth = 0.0;

    // Calculate the source-listener vector
    FloatPoint3D listenerPosition = listener()->position();
    FloatPoint3D sourceListener = m_position - listenerPosition;

    if (sourceListener.isZero()) {
        // degenerate case if source and listener are at the same point
        *outAzimuth = 0.0;
        *outElevation = 0.0;
        return;
    }

    sourceListener.normalize();

    // Align axes
    FloatPoint3D listenerFront = listener()->orientation();
    FloatPoint3D listenerUp = listener()->upVector();
    FloatPoint3D listenerRight = listenerFront.cross(listenerUp);
    listenerRight.normalize();

    FloatPoint3D listenerFrontNorm = listenerFront;
    listenerFrontNorm.normalize();

    FloatPoint3D up = listenerRight.cross(listenerFrontNorm);

    float upProjection = sourceListener.dot(up);

    FloatPoint3D projectedSource = sourceListener - upProjection * up;
    projectedSource.normalize();

    azimuth = 180.0 * acos(projectedSource.dot(listenerRight)) / piDouble;
    fixNANs(azimuth); // avoid illegal values

    // Source  in front or behind the listener
    double frontBack = projectedSource.dot(listenerFrontNorm);
    if (frontBack < 0.0)
        azimuth = 360.0 - azimuth;

    // Make azimuth relative to "front" and not "right" listener vector
    if ((azimuth >= 0.0) && (azimuth <= 270.0))
        azimuth = 90.0 - azimuth;
    else
        azimuth = 450.0 - azimuth;

    // Elevation
    double elevation = 90.0 - 180.0 * acos(sourceListener.dot(up)) / piDouble;
    fixNANs(elevation); // avoid illegal values

    if (elevation > 90.0)
        elevation = 180.0 - elevation;
    else if (elevation < -90.0)
        elevation = -180.0 - elevation;

    if (outAzimuth)
        *outAzimuth = azimuth;
    if (outElevation)
        *outElevation = elevation;
}

float PannerNode::dopplerRate()
{
    double dopplerShift = 1.0;

    // FIXME: optimize for case when neither source nor listener has changed...
    double dopplerFactor = listener()->dopplerFactor();

    if (dopplerFactor > 0.0) {
        double speedOfSound = listener()->speedOfSound();

        const FloatPoint3D &sourceVelocity = m_velocity;
        const FloatPoint3D &listenerVelocity = listener()->velocity();

        // Don't bother if both source and listener have no velocity
        bool sourceHasVelocity = !sourceVelocity.isZero();
        bool listenerHasVelocity = !listenerVelocity.isZero();

        if (sourceHasVelocity || listenerHasVelocity) {
            // Calculate the source to listener vector
            FloatPoint3D listenerPosition = listener()->position();
            FloatPoint3D sourceToListener = m_position - listenerPosition;

            double sourceListenerMagnitude = sourceToListener.length();

            double listenerProjection = sourceToListener.dot(listenerVelocity) / sourceListenerMagnitude;
            double sourceProjection = sourceToListener.dot(sourceVelocity) / sourceListenerMagnitude;

            listenerProjection = -listenerProjection;
            sourceProjection = -sourceProjection;

            double scaledSpeedOfSound = speedOfSound / dopplerFactor;
            listenerProjection = min(listenerProjection, scaledSpeedOfSound);
            sourceProjection = min(sourceProjection, scaledSpeedOfSound);

            dopplerShift = ((speedOfSound - dopplerFactor * listenerProjection) / (speedOfSound - dopplerFactor * sourceProjection));
            fixNANs(dopplerShift); // avoid illegal values

            // Limit the pitch shifting to 4 octaves up and 3 octaves down.
            if (dopplerShift > 16.0)
                dopplerShift = 16.0;
            else if (dopplerShift < 0.125)
                dopplerShift = 0.125;   
        }
    }

    return static_cast<float>(dopplerShift);
}

float PannerNode::distanceConeGain()
{
    FloatPoint3D listenerPosition = listener()->position();

    double listenerDistance = m_position.distanceTo(listenerPosition);
    double distanceGain = m_distanceEffect.gain(listenerDistance);
    
    m_distanceGain->setValue(static_cast<float>(distanceGain));

    // FIXME: could optimize by caching coneGain
    double coneGain = m_coneEffect.gain(m_position, m_orientation, listenerPosition);
    
    m_coneGain->setValue(static_cast<float>(coneGain));

    return float(distanceGain * coneGain);
}

void PannerNode::notifyAudioSourcesConnectedToNode(AudioNode* node)
{
    ASSERT(node);
    if (!node)
        return;
        
    // First check if this node is an AudioBufferSourceNode. If so, let it know about us so that doppler shift pitch can be taken into account.
    if (node->nodeType() == NodeTypeAudioBufferSource) {
        AudioBufferSourceNode* bufferSourceNode = reinterpret_cast<AudioBufferSourceNode*>(node);
        bufferSourceNode->setPannerNode(this);
    }
    else {
        // Go through all inputs to this node.
        for (unsigned i = 0; i < node->numberOfInputs(); ++i) {
            AudioNodeInput* input = node->input(i);

            // For each input, go through all of its connections, looking for AudioBufferSourceNodes.
            for (unsigned j = 0; j < input->numberOfRenderingConnections(); ++j) {
                AudioNodeOutput* connectedOutput = input->renderingOutput(j);
                AudioNode* connectedNode = connectedOutput->node();
                notifyAudioSourcesConnectedToNode(connectedNode); // recurse
            }
        }
    }
}

} // namespace WebCore
