/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "CCodec"
#include <cutils/properties.h>
#include <utils/Log.h>

#include <thread>

#include <C2Config.h>
#include <C2ParamInternal.h>
#include <C2PlatformSupport.h>
#include <C2V4l2Support.h>

#include <android/IOMXBufferSource.h>
#include <android/IGraphicBufferSource.h>
#include <cutils/properties.h>
#include <gui/bufferqueue/1.0/H2BGraphicBufferProducer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <media/omx/1.0/WOmx.h>
#include <media/stagefright/codec2/1.0/InputSurface.h>
#include <media/stagefright/BufferProducerWrapper.h>
#include <media/stagefright/PersistentSurface.h>

#include "C2OMXNode.h"
#include "CCodec.h"
#include "CCodecBufferChannel.h"
#include "InputSurfaceWrapper.h"

namespace android {

using namespace std::chrono_literals;
using ::android::hardware::graphics::bufferqueue::V1_0::utils::H2BGraphicBufferProducer;
using BGraphicBufferSource = ::android::IGraphicBufferSource;

namespace {

class CCodecWatchdog : public AHandler {
private:
    enum {
        kWhatRegister,
        kWhatWatch,
    };
    constexpr static int64_t kWatchIntervalUs = 3000000;  // 3 secs

public:
    static sp<CCodecWatchdog> getInstance() {
        Mutexed<sp<CCodecWatchdog>>::Locked instance(sInstance);
        if (*instance == nullptr) {
            *instance = new CCodecWatchdog;
            (*instance)->init();
        }
        return *instance;
    }

    ~CCodecWatchdog() = default;

    void registerCodec(CCodec *codec) {
        sp<AMessage> msg = new AMessage(kWhatRegister, this);
        msg->setPointer("codec", codec);
        msg->post();
    }

protected:
    void onMessageReceived(const sp<AMessage> &msg) {
        switch (msg->what()) {
            case kWhatRegister: {
                void *ptr = nullptr;
                CHECK(msg->findPointer("codec", &ptr));
                Mutexed<std::list<wp<CCodec>>>::Locked codecs(mCodecs);
                codecs->emplace_back((CCodec *)ptr);
                break;
            }

            case kWhatWatch: {
                Mutexed<std::list<wp<CCodec>>>::Locked codecs(mCodecs);
                for (auto it = codecs->begin(); it != codecs->end(); ) {
                    sp<CCodec> codec = it->promote();
                    if (codec == nullptr) {
                        it = codecs->erase(it);
                        continue;
                    }
                    codec->initiateReleaseIfStuck();
                    ++it;
                }
                msg->post(kWatchIntervalUs);
                break;
            }

            default: {
                TRESPASS("CCodecWatchdog: unrecognized message");
            }
        }
    }

private:
    CCodecWatchdog() : mLooper(new ALooper) {}

    void init() {
        mLooper->setName("CCodecWatchdog");
        mLooper->registerHandler(this);
        mLooper->start();
        (new AMessage(kWhatWatch, this))->post(kWatchIntervalUs);
    }

    static Mutexed<sp<CCodecWatchdog>> sInstance;

    sp<ALooper> mLooper;
    Mutexed<std::list<wp<CCodec>>> mCodecs;
};

Mutexed<sp<CCodecWatchdog>> CCodecWatchdog::sInstance;

class C2InputSurfaceWrapper : public InputSurfaceWrapper {
public:
    explicit C2InputSurfaceWrapper(
            const std::shared_ptr<Codec2Client::InputSurface> &surface) :
        mSurface(surface) {
    }

    ~C2InputSurfaceWrapper() override = default;

    status_t connect(const std::shared_ptr<Codec2Client::Component> &comp) override {
        if (mConnection != nullptr) {
            return ALREADY_EXISTS;
        }
        return static_cast<status_t>(
                mSurface->connectToComponent(comp, &mConnection));
    }

    void disconnect() override {
        if (mConnection != nullptr) {
            mConnection->disconnect();
            mConnection = nullptr;
        }
    }

private:
    std::shared_ptr<Codec2Client::InputSurface> mSurface;
    std::shared_ptr<Codec2Client::InputSurfaceConnection> mConnection;
};

class GraphicBufferSourceWrapper : public InputSurfaceWrapper {
public:
//    explicit GraphicBufferSourceWrapper(const sp<BGraphicBufferSource> &source) : mSource(source) {}
    GraphicBufferSourceWrapper(
            const sp<BGraphicBufferSource> &source,
            uint32_t width,
            uint32_t height)
        : mSource(source), mWidth(width), mHeight(height) {}
    ~GraphicBufferSourceWrapper() override = default;

    status_t connect(const std::shared_ptr<Codec2Client::Component> &comp) override {
        // TODO: proper color aspect & dataspace
        android_dataspace dataSpace = HAL_DATASPACE_BT709;

        mNode = new C2OMXNode(comp);
        mNode->setFrameSize(mWidth, mHeight);
        mSource->configure(mNode, dataSpace);

        // TODO: configure according to intf().

        sp<IOMXBufferSource> source = mNode->getSource();
        if (source == nullptr) {
            return NO_INIT;
        }
        constexpr size_t kNumSlots = 16;
        for (size_t i = 0; i < kNumSlots; ++i) {
            source->onInputBufferAdded(i);
        }
        source->onOmxExecuting();
        return OK;
    }

    void disconnect() override {
        if (mNode == nullptr) {
            return;
        }
        sp<IOMXBufferSource> source = mNode->getSource();
        if (source == nullptr) {
            ALOGD("GBSWrapper::disconnect: node is not configured with OMXBufferSource.");
            return;
        }
        source->onOmxIdle();
        source->onOmxLoaded();
        mNode.clear();
    }

private:
    sp<BGraphicBufferSource> mSource;
    sp<C2OMXNode> mNode;
    uint32_t mWidth;
    uint32_t mHeight;
};

}  // namespace

// CCodec::ClientListener

struct CCodec::ClientListener : public Codec2Client::Listener {

    explicit ClientListener(const wp<CCodec> &codec) : mCodec(codec) {}

    virtual void onWorkDone(
            const std::weak_ptr<Codec2Client::Component>& component,
            std::list<std::unique_ptr<C2Work>>& workItems) override {
        (void)component;
        sp<CCodec> codec(mCodec.promote());
        if (!codec) {
            return;
        }
        codec->onWorkDone(workItems);
    }

    virtual void onTripped(
            const std::weak_ptr<Codec2Client::Component>& component,
            const std::vector<std::shared_ptr<C2SettingResult>>& settingResult
            ) override {
        // TODO
        (void)component;
        (void)settingResult;
    }

    virtual void onError(
            const std::weak_ptr<Codec2Client::Component>& component,
            uint32_t errorCode) override {
        // TODO
        (void)component;
        (void)errorCode;
    }

    virtual void onDeath(
            const std::weak_ptr<Codec2Client::Component>& component) override {
        { // Log the death of the component.
            std::shared_ptr<Codec2Client::Component> comp = component.lock();
            if (!comp) {
                ALOGE("Codec2 component died.");
            } else {
                ALOGE("Codec2 component \"%s\" died.", comp->getName().c_str());
            }
        }

        // Report to MediaCodec.
        sp<CCodec> codec(mCodec.promote());
        if (!codec || !codec->mCallback) {
            return;
        }
        codec->mCallback->onError(DEAD_OBJECT, ACTION_CODE_FATAL);
    }

private:
    wp<CCodec> mCodec;
};

// CCodec

CCodec::CCodec()
    : mChannel(new CCodecBufferChannel([this] (status_t err, enum ActionCode actionCode) {
          mCallback->onError(err, actionCode);
      })) {
    CCodecWatchdog::getInstance()->registerCodec(this);
    initializeStandardParams();
}

CCodec::~CCodec() {
}

std::shared_ptr<BufferChannelBase> CCodec::getBufferChannel() {
    return mChannel;
}

status_t CCodec::tryAndReportOnError(std::function<status_t()> job) {
    status_t err = job();
    if (err != C2_OK) {
        mCallback->onError(err, ACTION_CODE_FATAL);
    }
    return err;
}

void CCodec::initiateAllocateComponent(const sp<AMessage> &msg) {
    auto setAllocating = [this] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != RELEASED) {
            return INVALID_OPERATION;
        }
        state->set(ALLOCATING);
        return OK;
    };
    if (tryAndReportOnError(setAllocating) != OK) {
        return;
    }

    sp<RefBase> codecInfo;
    CHECK(msg->findObject("codecInfo", &codecInfo));
    // For Codec 2.0 components, componentName == codecInfo->getCodecName().

    sp<AMessage> allocMsg(new AMessage(kWhatAllocate, this));
    allocMsg->setObject("codecInfo", codecInfo);
    allocMsg->post();
}

void CCodec::allocate(const sp<MediaCodecInfo> &codecInfo) {
    if (codecInfo == nullptr) {
        mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        return;
    }
    ALOGV("allocate(%s)", codecInfo->getCodecName());
    mClientListener.reset(new ClientListener(this));

    AString componentName = codecInfo->getCodecName();
    std::shared_ptr<Codec2Client> client;
    std::shared_ptr<Codec2Client::Component> comp =
            Codec2Client::CreateComponentByName(
            componentName.c_str(),
            mClientListener,
            &client);
    if (!comp) {
        ALOGE("Failed Create component: %s", componentName.c_str());
        Mutexed<State>::Locked state(mState);
        state->set(RELEASED);
        state.unlock();
        mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        state.lock();
        return;
    }
    ALOGV("Success Create component: %s", componentName.c_str());
    mChannel->setComponent(comp);
    auto setAllocated = [this, comp, client] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != ALLOCATING) {
            state->set(RELEASED);
            return UNKNOWN_ERROR;
        }
        state->set(ALLOCATED);
        state->comp = comp;
        mClient = client;
        return OK;
    };
    if (tryAndReportOnError(setAllocated) != OK) {
        return;
    }
    mCallback->onComponentAllocated(comp->getName().c_str());
}

void CCodec::initiateConfigureComponent(const sp<AMessage> &format) {
    auto checkAllocated = [this] {
        Mutexed<State>::Locked state(mState);
        return (state->get() != ALLOCATED) ? UNKNOWN_ERROR : OK;
    };
    if (tryAndReportOnError(checkAllocated) != OK) {
        return;
    }

    sp<AMessage> msg(new AMessage(kWhatConfigure, this));
    msg->setMessage("format", format);
    msg->post();
}

void CCodec::configure(const sp<AMessage> &msg) {
    std::shared_ptr<Codec2Client::Component> comp;
    auto checkAllocated = [this, &comp] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != ALLOCATED) {
            state->set(RELEASED);
            return UNKNOWN_ERROR;
        }
        comp = state->comp;
        return OK;
    };
    if (tryAndReportOnError(checkAllocated) != OK) {
        return;
    }

    sp<AMessage> inputFormat(new AMessage);
    sp<AMessage> outputFormat(new AMessage);
    std::vector<std::shared_ptr<C2ParamDescriptor>> paramDescs;

    auto doConfig = [=, paramDescsPtr = &paramDescs] {
        c2_status_t c2err = comp->querySupportedParams(paramDescsPtr);
        if (c2err != C2_OK) {
            ALOGD("Failed to query supported params");
            // TODO: return error once we complete implementation.
            // return UNKNOWN_ERROR;
        }

        AString mime;
        if (!msg->findString("mime", &mime)) {
            return BAD_VALUE;
        }

        int32_t encoder;
        if (!msg->findInt32("encoder", &encoder)) {
            encoder = false;
        }

        // TODO: read from intf()
        if ((!encoder) != (comp->getName().find("encoder") == std::string::npos)) {
            return UNKNOWN_ERROR;
        }

        sp<RefBase> obj;
        if (msg->findObject("native-window", &obj)) {
            sp<Surface> surface = static_cast<Surface *>(obj.get());
            setSurface(surface);
        }

        std::vector<std::unique_ptr<C2Param>> params;
        std::initializer_list<C2Param::Index> indices {
            C2PortMimeConfig::input::PARAM_TYPE,
            C2PortMimeConfig::output::PARAM_TYPE,
        };
        c2err = comp->query(
                {},
                indices,
                C2_DONT_BLOCK,
                &params);
        if (c2err != C2_OK) {
            ALOGE("Failed to query component interface: %d", c2err);
            return UNKNOWN_ERROR;
        }
        if (params.size() != indices.size()) {
            ALOGE("Component returns wrong number of params");
            return UNKNOWN_ERROR;
        }
        if (!params[0] || !params[1]) {
            ALOGE("Component returns null params");
            return UNKNOWN_ERROR;
        }
        inputFormat->setString("mime", ((C2PortMimeConfig *)params[0].get())->m.value);
        outputFormat->setString("mime", ((C2PortMimeConfig *)params[1].get())->m.value);

        // XXX: hack
        bool audio = mime.startsWithIgnoreCase("audio/");
        if (!audio) {
            int32_t tmp;
            if (msg->findInt32("width", &tmp)) {
                inputFormat->setInt32("width", tmp);
                outputFormat->setInt32("width", tmp);
            }
            if (msg->findInt32("height", &tmp)) {
                inputFormat->setInt32("height", tmp);
                outputFormat->setInt32("height", tmp);
            }
        } else {
            if (encoder) {
                inputFormat->setInt32("channel-count", 1);
                inputFormat->setInt32("sample-rate", 44100);
                outputFormat->setInt32("channel-count", 1);
                outputFormat->setInt32("sample-rate", 44100);
            } else {
                outputFormat->setInt32("channel-count", 2);
                outputFormat->setInt32("sample-rate", 44100);
            }
        }

        // TODO

        return OK;
    };
    if (tryAndReportOnError(doConfig) != OK) {
        return;
    }

    {
        Mutexed<Formats>::Locked formats(mFormats);
        formats->inputFormat = inputFormat;
        formats->outputFormat = outputFormat;
    }
    std::shared_ptr<C2ParamReflector> reflector = mClient->getParamReflector();
    if (reflector != nullptr) {
        Mutexed<ReflectedParamUpdater>::Locked paramUpdater(mParamUpdater);
        paramUpdater->clear();
        paramUpdater->addParamDesc(reflector, paramDescs);
    } else {
        ALOGE("Failed to get param reflector");
        // TODO: report error once we complete implementation.
    }
    mCallback->onComponentConfigured(inputFormat, outputFormat);
}

void CCodec::initiateCreateInputSurface() {
    status_t err = [this] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != ALLOCATED) {
            return UNKNOWN_ERROR;
        }
        // TODO: read it from intf() properly.
        if (state->comp->getName().find("encoder") == std::string::npos) {
            return INVALID_OPERATION;
        }
        return OK;
    }();
    if (err != OK) {
        mCallback->onInputSurfaceCreationFailed(err);
        return;
    }

    (new AMessage(kWhatCreateInputSurface, this))->post();
}

void CCodec::createInputSurface() {
    status_t err;
    sp<IGraphicBufferProducer> bufferProducer;

    sp<AMessage> inputFormat;
    sp<AMessage> outputFormat;
    {
        Mutexed<Formats>::Locked formats(mFormats);
        inputFormat = formats->inputFormat;
        outputFormat = formats->outputFormat;
    }

    // TODO: Remove this property check and assume it's always true.
    if (property_get_bool("debug.stagefright.c2inputsurface", false)) {
        std::shared_ptr<Codec2Client::InputSurface> surface;

        err = static_cast<status_t>(mClient->createInputSurface(&surface));
        if (err != OK) {
            ALOGE("Failed to create input surface: %d", static_cast<int>(err));
            mCallback->onInputSurfaceCreationFailed(err);
            return;
        }
        if (!surface) {
            ALOGE("Failed to create input surface: null input surface");
            mCallback->onInputSurfaceCreationFailed(UNKNOWN_ERROR);
            return;
        }
        bufferProducer = surface->getGraphicBufferProducer();
        err = setupInputSurface(std::make_shared<C2InputSurfaceWrapper>(surface));
    } else { // TODO: Remove this block.
        using namespace ::android::hardware::media::omx::V1_0;
        sp<IOmx> tOmx = IOmx::getService("default");
        if (tOmx == nullptr) {
            ALOGE("Failed to create input surface");
            mCallback->onInputSurfaceCreationFailed(UNKNOWN_ERROR);
            return;
        }
        sp<IOMX> omx = new utils::LWOmx(tOmx);

        sp<BGraphicBufferSource> bufferSource;
        err = omx->createInputSurface(&bufferProducer, &bufferSource);

        if (err != OK) {
            ALOGE("Failed to create input surface: %d", err);
            mCallback->onInputSurfaceCreationFailed(err);
            return;
        }
        int32_t width = 0;
        (void)outputFormat->findInt32("width", &width);
        int32_t height = 0;
        (void)outputFormat->findInt32("height", &height);
        err = setupInputSurface(std::make_shared<GraphicBufferSourceWrapper>(
                bufferSource, width, height));
    }

    if (err != OK) {
        ALOGE("Failed to set up input surface: %d", err);
        mCallback->onInputSurfaceCreationFailed(err);
        return;
    }
    mCallback->onInputSurfaceCreated(
            inputFormat,
            outputFormat,
            new BufferProducerWrapper(bufferProducer));
}

status_t CCodec::setupInputSurface(const std::shared_ptr<InputSurfaceWrapper> &surface) {
    status_t err = mChannel->setInputSurface(surface);
    if (err != OK) {
        return err;
    }

    // TODO: configure |surface| with other settings.
    return OK;
}

void CCodec::initiateSetInputSurface(const sp<PersistentSurface> &surface) {
    sp<AMessage> msg = new AMessage(kWhatSetInputSurface, this);
    msg->setObject("surface", surface);
    msg->post();
}

void CCodec::setInputSurface(const sp<PersistentSurface> &surface) {
    sp<AMessage> inputFormat;
    sp<AMessage> outputFormat;
    {
        Mutexed<Formats>::Locked formats(mFormats);
        inputFormat = formats->inputFormat;
        outputFormat = formats->outputFormat;
    }
    int32_t width = 0;
    (void)outputFormat->findInt32("width", &width);
    int32_t height = 0;
    (void)outputFormat->findInt32("height", &height);
    status_t err = setupInputSurface(std::make_shared<GraphicBufferSourceWrapper>(
            surface->getBufferSource(), width, height));
    if (err != OK) {
        ALOGE("Failed to set up input surface: %d", err);
        mCallback->onInputSurfaceDeclined(err);
        return;
    }
    mCallback->onInputSurfaceAccepted(inputFormat, outputFormat);
}

void CCodec::initiateStart() {
    auto setStarting = [this] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != ALLOCATED) {
            return UNKNOWN_ERROR;
        }
        state->set(STARTING);
        return OK;
    };
    if (tryAndReportOnError(setStarting) != OK) {
        return;
    }

    (new AMessage(kWhatStart, this))->post();
}

void CCodec::start() {
    std::shared_ptr<Codec2Client::Component> comp;
    auto checkStarting = [this, &comp] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != STARTING) {
            return UNKNOWN_ERROR;
        }
        comp = state->comp;
        return OK;
    };
    if (tryAndReportOnError(checkStarting) != OK) {
        return;
    }

    c2_status_t err = comp->start();
    if (err != C2_OK) {
        // TODO: convert err into status_t
        mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
        return;
    }
    sp<AMessage> inputFormat;
    sp<AMessage> outputFormat;
    {
        Mutexed<Formats>::Locked formats(mFormats);
        inputFormat = formats->inputFormat;
        outputFormat = formats->outputFormat;
    }
    status_t err2 = mChannel->start(inputFormat, outputFormat);
    if (err2 != OK) {
        mCallback->onError(err2, ACTION_CODE_FATAL);
        return;
    }

    auto setRunning = [this] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != STARTING) {
            return UNKNOWN_ERROR;
        }
        state->set(RUNNING);
        return OK;
    };
    if (tryAndReportOnError(setRunning) != OK) {
        return;
    }
    mCallback->onStartCompleted();
}

void CCodec::initiateShutdown(bool keepComponentAllocated) {
    if (keepComponentAllocated) {
        initiateStop();
    } else {
        initiateRelease();
    }
}

void CCodec::initiateStop() {
    {
        Mutexed<State>::Locked state(mState);
        if (state->get() == ALLOCATED
                || state->get()  == RELEASED
                || state->get() == STOPPING
                || state->get() == RELEASING) {
            // We're already stopped, released, or doing it right now.
            state.unlock();
            mCallback->onStopCompleted();
            state.lock();
            return;
        }
        state->set(STOPPING);
    }

    (new AMessage(kWhatStop, this))->post();
}

void CCodec::stop() {
    std::shared_ptr<Codec2Client::Component> comp;
    {
        Mutexed<State>::Locked state(mState);
        if (state->get() == RELEASING) {
            state.unlock();
            // We're already stopped or release is in progress.
            mCallback->onStopCompleted();
            state.lock();
            return;
        } else if (state->get() != STOPPING) {
            state.unlock();
            mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
            state.lock();
            return;
        }
        comp = state->comp;
    }
    mChannel->stop();
    status_t err = comp->stop();
    if (err != C2_OK) {
        // TODO: convert err into status_t
        mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
    }

    {
        Mutexed<State>::Locked state(mState);
        if (state->get() == STOPPING) {
            state->set(ALLOCATED);
        }
    }
    mCallback->onStopCompleted();
}

void CCodec::initiateRelease(bool sendCallback /* = true */) {
    {
        Mutexed<State>::Locked state(mState);
        if (state->get() == RELEASED || state->get() == RELEASING) {
            // We're already released or doing it right now.
            if (sendCallback) {
                state.unlock();
                mCallback->onReleaseCompleted();
                state.lock();
            }
            return;
        }
        if (state->get() == ALLOCATING) {
            state->set(RELEASING);
            // With the altered state allocate() would fail and clean up.
            if (sendCallback) {
                state.unlock();
                mCallback->onReleaseCompleted();
                state.lock();
            }
            return;
        }
        state->set(RELEASING);
    }

    std::thread([this, sendCallback] { release(sendCallback); }).detach();
}

void CCodec::release(bool sendCallback) {
    std::shared_ptr<Codec2Client::Component> comp;
    {
        Mutexed<State>::Locked state(mState);
        if (state->get() == RELEASED) {
            if (sendCallback) {
                state.unlock();
                mCallback->onReleaseCompleted();
                state.lock();
            }
            return;
        }
        comp = state->comp;
    }
    mChannel->stop();
    comp->release();

    {
        Mutexed<State>::Locked state(mState);
        state->set(RELEASED);
        state->comp.reset();
    }
    if (sendCallback) {
        mCallback->onReleaseCompleted();
    }
}

status_t CCodec::setSurface(const sp<Surface> &surface) {
    return mChannel->setSurface(surface);
}

void CCodec::signalFlush() {
    status_t err = [this] {
        Mutexed<State>::Locked state(mState);
        if (state->get() == FLUSHED) {
            return ALREADY_EXISTS;
        }
        if (state->get() != RUNNING) {
            return UNKNOWN_ERROR;
        }
        state->set(FLUSHING);
        return OK;
    }();
    switch (err) {
        case ALREADY_EXISTS:
            mCallback->onFlushCompleted();
            return;
        case OK:
            break;
        default:
            mCallback->onError(err, ACTION_CODE_FATAL);
            return;
    }

    (new AMessage(kWhatFlush, this))->post();
}

void CCodec::flush() {
    std::shared_ptr<Codec2Client::Component> comp;
    auto checkFlushing = [this, &comp] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != FLUSHING) {
            return UNKNOWN_ERROR;
        }
        comp = state->comp;
        return OK;
    };
    if (tryAndReportOnError(checkFlushing) != OK) {
        return;
    }

    mChannel->stop();

    std::list<std::unique_ptr<C2Work>> flushedWork;
    c2_status_t err = comp->flush(C2Component::FLUSH_COMPONENT, &flushedWork);
    if (err != C2_OK) {
        // TODO: convert err into status_t
        mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
    }

    mChannel->flush(flushedWork);

    {
        Mutexed<State>::Locked state(mState);
        state->set(FLUSHED);
    }
    mCallback->onFlushCompleted();
}

void CCodec::signalResume() {
    auto setResuming = [this] {
        Mutexed<State>::Locked state(mState);
        if (state->get() != FLUSHED) {
            return UNKNOWN_ERROR;
        }
        state->set(RESUMING);
        return OK;
    };
    if (tryAndReportOnError(setResuming) != OK) {
        return;
    }

    (void)mChannel->start(nullptr, nullptr);

    {
        Mutexed<State>::Locked state(mState);
        if (state->get() != RESUMING) {
            state.unlock();
            mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
            state.lock();
            return;
        }
        state->set(RUNNING);
    }
}

void CCodec::signalSetParameters(const sp<AMessage> &params) {
    sp<AMessage> msg = new AMessage(kWhatSetParameters, this);
    msg->setMessage("params", params);
    msg->post();
}

void CCodec::initializeStandardParams() {
    mStandardParams.emplace("bitrate",          "coded.bitrate.value");
    mStandardParams.emplace("video-bitrate",    "coded.bitrate.value");
    mStandardParams.emplace("bitrate-mode",     "coded.bitrate-mode.value");
    mStandardParams.emplace("frame-rate",       "coded.frame-rate.value");
    mStandardParams.emplace("max-input-size",   "coded.max-frame-size.value");
    mStandardParams.emplace("rotation-degrees", "coded.vui.rotation.value");

    mStandardParams.emplace("prepend-sps-pps-to-idr-frames", "coding.add-csd-to-sync-frames.value");
    mStandardParams.emplace("i-frame-period",   "coding.gop.intra-period");
    mStandardParams.emplace("intra-refresh-period", "coding.intra-refresh.period");
    mStandardParams.emplace("quality",          "coding.quality.value");
    mStandardParams.emplace("request-sync",     "coding.request-sync.value");

    mStandardParams.emplace("operating-rate",   "ctrl.operating-rate.value");
    mStandardParams.emplace("priority",         "ctrl.priority.value");

    mStandardParams.emplace("channel-count",    "raw.channel-count.value");
    mStandardParams.emplace("max-width",        "raw.max-size.width");
    mStandardParams.emplace("max-height",       "raw.max-size.height");
    mStandardParams.emplace("pcm-encoding",     "raw.pcm-encoding.value");
    mStandardParams.emplace("color-format",     "raw.pixel-format.value");
    mStandardParams.emplace("sample-rate",      "raw.sample-rate.value");
    mStandardParams.emplace("width",            "raw.size.width");
    mStandardParams.emplace("height",           "raw.size.height");

    mStandardParams.emplace("is-adts",          "coded.aac-stream-format.value");

    // mStandardParams.emplace("stride", "raw.??");
    // mStandardParams.emplace("slice-height", "raw.??");
}

sp<AMessage> CCodec::filterParameters(const sp<AMessage> &params) const {
    sp<AMessage> filtered = params->dup();

    // TODO: some params may require recalculation or a type fix
    // e.g. i-frame-interval here
    {
        int32_t frameRateInt;
        if (filtered->findInt32("frame-rate", &frameRateInt)) {
            filtered->removeEntryAt(filtered->findEntryByName("frame-rate"));
            filtered->setFloat("frame-rate", frameRateInt);
        }
    }

    {
        float frameRate;
        int32_t iFrameInterval;
        if (filtered->findInt32("i-frame-interval", &iFrameInterval)
                && filtered->findFloat("frame-rate", &frameRate)) {
            filtered->setInt32("i-frame-period", iFrameInterval * frameRate + 0.5);
        }
    }

    {
        int32_t isAdts;
        if (filtered->findInt32("is-adts", &isAdts)) {
            filtered->setInt32(
                    "is-adts",
                    isAdts ? C2AacStreamFormatAdts : C2AacStreamFormatRaw);
        }
    }

    for (size_t ix = 0; ix < filtered->countEntries();) {
        AMessage::Type type;
        AString name = filtered->getEntryNameAt(ix, &type);
        if (name.startsWith("vendor.")) {
            // vendor params pass through as is
            ++ix;
            continue;
        }
        auto it = mStandardParams.find(name.c_str());
        if (it == mStandardParams.end()) {
            // non-standard parameters are filtered out
            filtered->removeEntryAt(ix);
            continue;
        }
        filtered->setEntryNameAt(ix++, it->second.c_str());
    }
    ALOGV("filtered %s to %s", params->debugString(4).c_str(), filtered->debugString(4).c_str());
    return filtered;
}

void CCodec::setParameters(const sp<AMessage> &unfiltered) {
    std::shared_ptr<Codec2Client::Component> comp;
    auto checkState = [this, &comp] {
        Mutexed<State>::Locked state(mState);
        if (state->get() == RELEASED) {
            return INVALID_OPERATION;
        }
        comp = state->comp;
        return OK;
    };
    if (tryAndReportOnError(checkState) != OK) {
        return;
    }

    sp<AMessage> params = filterParameters(unfiltered);

    c2_status_t err = C2_OK;
    std::vector<std::unique_ptr<C2Param>> vec;
    {
        Mutexed<ReflectedParamUpdater>::Locked paramUpdater(mParamUpdater);
        std::vector<C2Param::Index> indices;
        paramUpdater->getParamIndicesFromMessage(params, &indices);

        paramUpdater.unlock();
        if (indices.empty()) {
            ALOGD("no recognized params in: %s", params->debugString().c_str());
            return;
        }
        err = comp->query({}, indices, C2_MAY_BLOCK, &vec);
        if (err != C2_OK) {
            ALOGD("query failed with %d", err);
            // This is non-fatal.
            return;
        }
        paramUpdater.lock();

        paramUpdater->updateParamsFromMessage(params, &vec);
    }

    std::vector<C2Param *> paramVector;
    for (const std::unique_ptr<C2Param> &param : vec) {
        paramVector.push_back(param.get());
    }
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    err = comp->config(paramVector, C2_MAY_BLOCK, &failures);
    if (err != C2_OK) {
        ALOGD("config failed with %d", err);
        // This is non-fatal.
    }
}

void CCodec::signalEndOfInputStream() {
    // TODO
    mCallback->onSignaledInputEOS(INVALID_OPERATION);
}

void CCodec::signalRequestIDRFrame() {
    // TODO
}

void CCodec::onWorkDone(std::list<std::unique_ptr<C2Work>> &workItems) {
    Mutexed<std::list<std::unique_ptr<C2Work>>>::Locked queue(mWorkDoneQueue);
    queue->splice(queue->end(), workItems);
    (new AMessage(kWhatWorkDone, this))->post();
}

void CCodec::onMessageReceived(const sp<AMessage> &msg) {
    TimePoint now = std::chrono::steady_clock::now();
    switch (msg->what()) {
        case kWhatAllocate: {
            // C2ComponentStore::createComponent() should return within 100ms.
            setDeadline(now + 150ms, "allocate");
            sp<RefBase> obj;
            CHECK(msg->findObject("codecInfo", &obj));
            allocate((MediaCodecInfo *)obj.get());
            break;
        }
        case kWhatConfigure: {
            // C2Component::commit_sm() should return within 5ms.
            setDeadline(now + 50ms, "configure");
            sp<AMessage> format;
            CHECK(msg->findMessage("format", &format));
            configure(format);
            setParameters(format);
            break;
        }
        case kWhatStart: {
            // C2Component::start() should return within 500ms.
            setDeadline(now + 550ms, "start");
            start();
            break;
        }
        case kWhatStop: {
            // C2Component::stop() should return within 500ms.
            setDeadline(now + 550ms, "stop");
            stop();
            break;
        }
        case kWhatFlush: {
            // C2Component::flush_sm() should return within 5ms.
            setDeadline(now + 50ms, "flush");
            flush();
            break;
        }
        case kWhatCreateInputSurface: {
            // Surface operations may be briefly blocking.
            setDeadline(now + 100ms, "createInputSurface");
            createInputSurface();
            break;
        }
        case kWhatSetInputSurface: {
            // Surface operations may be briefly blocking.
            setDeadline(now + 100ms, "setInputSurface");
            sp<RefBase> obj;
            CHECK(msg->findObject("surface", &obj));
            sp<PersistentSurface> surface(static_cast<PersistentSurface *>(obj.get()));
            setInputSurface(surface);
            break;
        }
        case kWhatSetParameters: {
            setDeadline(now + 50ms, "setParameters");
            sp<AMessage> params;
            CHECK(msg->findMessage("params", &params));
            setParameters(params);
            break;
        }
        case kWhatWorkDone: {
            std::unique_ptr<C2Work> work;
            {
                Mutexed<std::list<std::unique_ptr<C2Work>>>::Locked queue(mWorkDoneQueue);
                if (queue->empty()) {
                    break;
                }
                work.swap(queue->front());
                queue->pop_front();
                if (!queue->empty()) {
                    (new AMessage(kWhatWorkDone, this))->post();
                }
            }
            mChannel->onWorkDone(work);
            break;
        }
        default: {
            ALOGE("unrecognized message");
            break;
        }
    }
    setDeadline(TimePoint::max(), "none");
}

void CCodec::setDeadline(const TimePoint &newDeadline, const char *name) {
    Mutexed<NamedTimePoint>::Locked deadline(mDeadline);
    deadline->set(newDeadline, name);
}

void CCodec::initiateReleaseIfStuck() {
    std::string name;
    {
        Mutexed<NamedTimePoint>::Locked deadline(mDeadline);
        if (deadline->get() >= std::chrono::steady_clock::now()) {
            // We're not stuck.
            return;
        }
        name = deadline->getName();
    }

    ALOGW("previous call to %s exceeded timeout", name.c_str());
    initiateRelease(false);
    mCallback->onError(UNKNOWN_ERROR, ACTION_CODE_FATAL);
}

}  // namespace android

extern "C" android::CodecBase *CreateCodec() {
    return new android::CCodec;
}

