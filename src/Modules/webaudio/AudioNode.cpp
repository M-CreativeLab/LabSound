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
#include "AudioNode.h"

#include "AudioContext.h"
#include "AudioNodeInput.h"
#include "AudioNodeOutput.h"
#include "AudioParam.h"
#include "WTF/Atomics.h"
#include "WTF/MainThread.h"

#if DEBUG_AUDIONODE_REFERENCES
#include <stdio.h>
#endif

namespace WebCore {

AudioNode::AudioNode(std::shared_ptr<AudioContext> context, float sampleRate)
    : m_isInitialized(false)
    , m_nodeType(NodeTypeUnknown)
    , m_context(context)
    , m_sampleRate(sampleRate)
    , m_lastProcessingTime(-1)
    , m_lastNonSilentTime(-1)
    , m_normalRefCount(1) // start out with normal refCount == 1 (like WTF::RefCounted class)
    , m_connectionRefCount(0)
    , m_isMarkedForDeletion(false)
    , m_isDisabled(false)
{
#if DEBUG_AUDIONODE_REFERENCES
    if (!s_isNodeCountInitialized) {
        s_isNodeCountInitialized = true;
        atexit(AudioNode::printNodeCounts);
    }
#endif
}

AudioNode::~AudioNode()
{
#if DEBUG_AUDIONODE_REFERENCES
    --s_nodeCount[nodeType()];
    fprintf(stderr, "%p: %d: AudioNode::~AudioNode() %d %d\n", this, nodeType(), m_normalRefCount, m_connectionRefCount);
#endif
}

void AudioNode::initialize()
{
    m_isInitialized = true;
}

void AudioNode::uninitialize()
{
    m_isInitialized = false;
}

void AudioNode::setNodeType(NodeType type)
{
    m_nodeType = type;

#if DEBUG_AUDIONODE_REFERENCES
    ++s_nodeCount[type];
#endif
}

void AudioNode::lazyInitialize()
{
    if (!isInitialized())
        initialize();
}

void AudioNode::addInput(std::unique_ptr<AudioNodeInput> input)
{
    m_inputs.emplace_back(std::move(input));
}

void AudioNode::addOutput(std::unique_ptr<AudioNodeOutput> output)
{
    m_outputs.emplace_back(std::move(output));
}

AudioNodeInput* AudioNode::input(unsigned i)
{
    if (i < m_inputs.size())
        return m_inputs[i].get();
    return 0;
}

AudioNodeOutput* AudioNode::output(unsigned i)
{
    if (i < m_outputs.size())
        return m_outputs[i].get();
    return 0;
}

void AudioNode::connect(AudioNode* destination, unsigned outputIndex, unsigned inputIndex, ExceptionCode& ec)
{
    ASSERT(!context().expired() && isMainThread());
    std::shared_ptr<AudioContext> ac = context().lock();
    AudioContext::AutoLocker locker(ac.get());

    if (!destination) {
        ec = SYNTAX_ERR;
        return;
    }

    // Sanity check input and output indices.
    if (outputIndex >= numberOfOutputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    if (destination && inputIndex >= destination->numberOfInputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    if (ac != destination->context().lock()) {
        ec = SYNTAX_ERR;
        return;
    }

    AudioNodeInput* input = destination->input(inputIndex);
    AudioNodeOutput* output = this->output(outputIndex);
    input->connect(output);

    // Let context know that a connection has been made.
    ac->incrementConnectionCount();
}

void AudioNode::connect(AudioParam* param, unsigned outputIndex, ExceptionCode& ec)
{
    ASSERT(!context().expired() && isMainThread());
    std::shared_ptr<AudioContext> ac = context().lock();
    AudioContext::AutoLocker locker(ac.get());

    if (!param) {
        ec = SYNTAX_ERR;
        return;
    }

    if (outputIndex >= numberOfOutputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    if (ac != param->context().lock()) {
        ec = SYNTAX_ERR;
        return;
    }

    AudioNodeOutput* output = this->output(outputIndex);
    param->connect(output);
}

void AudioNode::disconnect(unsigned outputIndex, ExceptionCode& ec)
{
    ASSERT(!context().expired() && isMainThread());
    std::shared_ptr<AudioContext> ac = context().lock();
    AudioContext::AutoLocker locker(ac.get());

    // Sanity check input and output indices.
    if (outputIndex >= numberOfOutputs()) {
        ec = INDEX_SIZE_ERR;
        return;
    }

    AudioNodeOutput* output = this->output(outputIndex);
    output->disconnectAll();
}

void AudioNode::processIfNecessary(size_t framesToProcess)
{
    if (context().expired())
        return;
    
    std::shared_ptr<AudioContext> ac = context().lock();
    ASSERT(ac->isAudioThread());
    
    if (!isInitialized())
        return;

    // Ensure that we only process once per rendering quantum.
    // This handles the "fanout" problem where an output is connected to multiple inputs.
    // The first time we're called during this time slice we process, but after that we don't want to re-process,
    // instead our output(s) will already have the results cached in their bus;
    double currentTime = ac->currentTime();
    if (m_lastProcessingTime != currentTime) {
        m_lastProcessingTime = currentTime; // important to first update this time because of feedback loops in the rendering graph

        pullInputs(framesToProcess);

        bool silentInputs = inputsAreSilent();
        if (!silentInputs)
            m_lastNonSilentTime = (ac->currentSampleFrame() + framesToProcess) / static_cast<double>(m_sampleRate);

        bool ps = propagatesSilence();
        if (silentInputs && ps)
            silenceOutputs();
        else {
            process(framesToProcess);
            unsilenceOutputs();
        }
    }
}

void AudioNode::checkNumberOfChannelsForInput(AudioNodeInput* input)
{
    ASSERT(!context().expired());
    std::shared_ptr<AudioContext> ac = context().lock();
    ASSERT(ac->isAudioThread() && ac->isGraphOwner());
    for (auto &i : m_inputs) {
        if (i.get() == input) {
            input->updateInternalBus();
            break;
        }
    }
}

bool AudioNode::propagatesSilence() const
{
    ASSERT(!context().expired());
    std::shared_ptr<AudioContext> ac = context().lock();
    return m_lastNonSilentTime + latencyTime() + tailTime() < ac->currentTime();
}

void AudioNode::pullInputs(size_t framesToProcess)
{
    ASSERT(!context().expired());
    std::shared_ptr<AudioContext> ac = context().lock();
    ASSERT(ac->isAudioThread());
    
    // Process all of the AudioNodes connected to our inputs.
    for (unsigned i = 0; i < m_inputs.size(); ++i)
        input(i)->pull(0, framesToProcess);
}

bool AudioNode::inputsAreSilent()
{
    for (unsigned i = 0; i < m_inputs.size(); ++i) {
        if (!input(i)->bus()->isSilent())
            return false;
    }
    return true;
}

void AudioNode::silenceOutputs()
{
    for (unsigned i = 0; i < m_outputs.size(); ++i)
        output(i)->bus()->zero();
}

void AudioNode::unsilenceOutputs()
{
    for (unsigned i = 0; i < m_outputs.size(); ++i) {
        output(i)->bus()->clearSilentFlag();
    }
}

void AudioNode::enableOutputsIfNecessary()
{
    if (m_isDisabled && m_connectionRefCount > 0) {
        ASSERT(isMainThread());
        auto ac = context().lock();
        AudioContext::AutoLocker locker(ac.get());

        m_isDisabled = false;
        for (unsigned i = 0; i < m_outputs.size(); ++i)
            output(i)->enable();
    }
}

void AudioNode::disableOutputsIfNecessary()
{
    // Disable outputs if appropriate. We do this if the number of connections is 0 or 1. The case
    // of 0 is from finishDeref() where there are no connections left. The case of 1 is from
    // AudioNodeInput::disable() where we want to disable outputs when there's only one connection
    // left because we're ready to go away, but can't quite yet.
    if (m_connectionRefCount <= 1 && !m_isDisabled) {
        // Still may have JavaScript references, but no more "active" connection references, so put all of our outputs in a "dormant" disabled state.
        // Garbage collection may take a very long time after this time, so the "dormant" disabled nodes should not bog down the rendering...

        // As far as JavaScript is concerned, our outputs must still appear to be connected.
        // But internally our outputs should be disabled from the inputs they're connected to.
        // disable() can recursively deref connections (and call disable()) down a whole chain of connected nodes.

        // FIXME: we special case the convolver and delay since they have a significant tail-time and shouldn't be disconnected simply
        // because they no longer have any input connections. This needs to be handled more generally where AudioNodes have
        // a tailTime attribute. Then the AudioNode only needs to remain "active" for tailTime seconds after there are no
        // longer any active connections.
        if (nodeType() != NodeTypeConvolver && nodeType() != NodeTypeDelay) {
            m_isDisabled = true;
            for (unsigned i = 0; i < m_outputs.size(); ++i)
                output(i)->disable();
        }
    }
}

void AudioNode::ref(RefType refType)
{
    switch (refType) {
    case RefTypeNormal:
        atomicIncrement(&m_normalRefCount);
        break;
    case RefTypeConnection:
        atomicIncrement(&m_connectionRefCount);
        break;
    default:
        ASSERT_NOT_REACHED();
    }

#if DEBUG_AUDIONODE_REFERENCES
    fprintf(stderr, "%p: %d: AudioNode::ref(%d) %d %d\n", this, nodeType(), refType, m_normalRefCount, m_connectionRefCount);
#endif

    // See the disabling code in finishDeref() below. This handles the case where a node
    // is being re-connected after being used at least once and disconnected.
    // In this case, we need to re-enable.
    if (refType == RefTypeConnection)
        enableOutputsIfNecessary();
}

void AudioNode::deref(RefType refType)
{
    if (context().expired()) {
        // If the context is gone already, there's not much to do.
        finishDeref(refType);
        return;
    }
    
    // The actual work for deref happens completely within the audio context's graph lock.
    // In the case of the audio thread, we must use a tryLock to avoid glitches.
    bool hasLock = false;
    bool mustReleaseLock = false;
    
    ASSERT(!context().expired());
    auto ac = context().lock();
    
    if (ac->isAudioThread()) {
        // Real-time audio thread must not contend lock (to avoid glitches).
        hasLock = ac->tryLock(mustReleaseLock);
    } else {
        ac->lock(mustReleaseLock);
        hasLock = true;
    }
    
    if (hasLock) {
        // This is where the real deref work happens.
        finishDeref(refType);

        if (mustReleaseLock)
            ac->unlock();
    } else {
        // We were unable to get the lock, so put this in a list to finish up later.
        ASSERT(ac->isAudioThread());
        ASSERT(refType == RefTypeConnection);
        ac->addDeferredFinishDeref(this);
    }

    // Once AudioContext::uninitialize() is called there's no more chances for deleteMarkedNodes() to get called, so we call here.
    // We can't call in AudioContext::~AudioContext() since it will never be called as long as any AudioNode is alive
    // because AudioNodes keep a reference to the context.
    if (ac->isAudioThreadFinished())
        ac->deleteMarkedNodes();
}

void AudioNode::finishDeref(RefType refType)
{
    bool shuttingDown = context().expired();
    auto ac = context().lock();
    
    if (!shuttingDown) {
        ASSERT(ac->isGraphOwner());
    }
    
    switch (refType) {
    case RefTypeNormal:
        ASSERT(m_normalRefCount > 0);
        atomicDecrement(&m_normalRefCount);
        break;
    case RefTypeConnection:
        ASSERT(m_connectionRefCount > 0);
        atomicDecrement(&m_connectionRefCount);
        break;
    default:
        ASSERT_NOT_REACHED();
    }
    
#if DEBUG_AUDIONODE_REFERENCES
    fprintf(stderr, "%p: %d: AudioNode::deref(%d) %d %d\n", this, nodeType(), refType, m_normalRefCount, m_connectionRefCount);
#endif

    if (!m_connectionRefCount) {
        if (!m_normalRefCount) {
            if (!m_isMarkedForDeletion) {
                // All references are gone - we need to go away.
                for (unsigned i = 0; i < m_outputs.size(); ++i)
                    output(i)->disconnectAll(); // This will deref() nodes we're connected to.

                // Mark for deletion at end of each render quantum or when context shuts down.
                if (!shuttingDown)
                    ac->markForDeletion(this);
                
                m_isMarkedForDeletion = true;

            }
        } else if (refType == RefTypeConnection)
            disableOutputsIfNecessary();
    }
}

#if DEBUG_AUDIONODE_REFERENCES

bool AudioNode::s_isNodeCountInitialized = false;
int AudioNode::s_nodeCount[NodeTypeEnd];

void AudioNode::printNodeCounts()
{
    fprintf(stderr, "\n\n");
    fprintf(stderr, "===========================\n");
    fprintf(stderr, "AudioNode: reference counts\n");
    fprintf(stderr, "===========================\n");

    for (unsigned i = 0; i < NodeTypeEnd; ++i)
        fprintf(stderr, "%d: %d\n", i, s_nodeCount[i]);

    fprintf(stderr, "===========================\n\n\n");
}

#endif // DEBUG_AUDIONODE_REFERENCES

} // namespace WebCore
