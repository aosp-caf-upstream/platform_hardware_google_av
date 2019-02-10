/*
 * Copyright (C) 2018 The Android Open Source Project
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

// #define LOG_NDEBUG 0
#define LOG_TAG "codec2_hidl_hal_audio_enc_test"

#include <android-base/logging.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <fstream>

#include <codec2/hidl/client.h>
#include <C2AllocatorIon.h>
#include <C2Config.h>
#include <C2Debug.h>
#include <C2Buffer.h>
#include <C2BufferPriv.h>

using android::C2AllocatorIon;

#include <VtsHalHidlTargetTestBase.h>
#include "media_c2_audio_hidl_test_common.h"
#include "media_c2_hidl_test_common.h"

class LinearBuffer : public C2Buffer {
   public:
    explicit LinearBuffer(const std::shared_ptr<C2LinearBlock>& block)
        : C2Buffer(
              {block->share(block->offset(), block->size(), ::C2Fence())}) {}
};

static ComponentTestEnvironment* gEnv = nullptr;

namespace {

class Codec2AudioEncHidlTest : public ::testing::VtsHalHidlTargetTestBase {
   private:
    typedef ::testing::VtsHalHidlTargetTestBase Super;

   public:
    ::std::string getTestCaseInfo() const override {
        return ::std::string() +
                "Component: " + gEnv->getComponent().c_str() + " | " +
                "Instance: " + gEnv->getInstance().c_str() + " | " +
                "Res: " + gEnv->getRes().c_str();
    }

    // google.codec2 Audio test setup
    virtual void SetUp() override {
        Super::SetUp();
        mDisableTest = false;
        ALOGV("Codec2AudioEncHidlTest SetUp");
        mClient = android::Codec2Client::CreateFromService(
            gEnv->getInstance().c_str());
        ASSERT_NE(mClient, nullptr);
        mListener.reset(new CodecListener(
            [this](std::list<std::unique_ptr<C2Work>>& workItems) {
                handleWorkDone(workItems);
            }));
        ASSERT_NE(mListener, nullptr);
        for (int i = 0; i < MAX_INPUT_BUFFERS; ++i) {
            mWorkQueue.emplace_back(new C2Work);
        }
        mClient->createComponent(gEnv->getComponent().c_str(), mListener,
                                 &mComponent);
        ASSERT_NE(mComponent, nullptr);

        std::shared_ptr<C2AllocatorStore> store =
            android::GetCodec2PlatformAllocatorStore();
        CHECK_EQ(store->fetchAllocator(C2AllocatorStore::DEFAULT_LINEAR,
                                       &mLinearAllocator),
                 C2_OK);
        mLinearPool = std::make_shared<C2PooledBlockPool>(mLinearAllocator,
                                                          mBlockPoolId++);
        ASSERT_NE(mLinearPool, nullptr);

        mCompName = unknown_comp;
        struct StringToName {
            const char* Name;
            standardComp CompName;
        };
        const StringToName kStringToName[] = {
            {"aac", aac},
            {"flac", flac},
            {"amrnb", amrnb},
            {"amrwb", amrwb},
        };
        const size_t kNumStringToName =
            sizeof(kStringToName) / sizeof(kStringToName[0]);

        std::string substring;
        std::string comp;
        substring = std::string(gEnv->getComponent());
        /* TODO: better approach to find the component */
        /* "c2.android." => 11th position */
        size_t pos = 11;
        size_t len = substring.find(".encoder", pos);
        comp = substring.substr(pos, len - pos);

        for (size_t i = 0; i < kNumStringToName; ++i) {
            if (!strcasecmp(comp.c_str(), kStringToName[i].Name)) {
                mCompName = kStringToName[i].CompName;
                break;
            }
        }
        mEos = false;
        mCsd = false;
        mFramesReceived = 0;
        if (mCompName == unknown_comp) mDisableTest = true;
        if (mDisableTest) std::cout << "[   WARN   ] Test Disabled \n";
    }

    virtual void TearDown() override {
        if (mComponent != nullptr) {
            if (::testing::Test::HasFatalFailure()) return;
            mComponent->release();
            mComponent = nullptr;
        }
        Super::TearDown();
    }
    // callback function to process onWorkDone received by Listener
    void handleWorkDone(std::list<std::unique_ptr<C2Work>>& workItems) {
        for (std::unique_ptr<C2Work>& work : workItems) {
            // handle configuration changes in work done
            if (!work->worklets.empty() &&
                (work->worklets.front()->output.configUpdate.size() != 0)) {
                ALOGV("Config Update");
                std::vector<std::unique_ptr<C2Param>> updates =
                    std::move(work->worklets.front()->output.configUpdate);
                std::vector<C2Param*> configParam;
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                for (size_t i = 0; i < updates.size(); ++i) {
                    C2Param* param = updates[i].get();
                    if (param->index() == C2StreamCsdInfo::output::PARAM_TYPE) {
                        mCsd = true;
                    }
                }
            }
            mFramesReceived++;
            mEos = (work->worklets.front()->output.flags &
                    C2FrameData::FLAG_END_OF_STREAM) != 0;
            ALOGV("WorkDone: frameID received %d",
                (int)work->worklets.front()->output.ordinal.frameIndex.peeku());
            work->input.buffers.clear();
            work->worklets.clear();
            typedef std::unique_lock<std::mutex> ULock;
            ULock l(mQueueLock);
            mWorkQueue.push_back(std::move(work));
            mQueueCondition.notify_all();
        }
    }
    enum standardComp {
        aac,
        flac,
        amrnb,
        amrwb,
        unknown_comp,
    };

    bool mEos;
    bool mCsd;
    bool mDisableTest;
    standardComp mCompName;
    uint32_t mFramesReceived;
    C2BlockPool::local_id_t mBlockPoolId;
    std::shared_ptr<C2BlockPool> mLinearPool;
    std::shared_ptr<C2Allocator> mLinearAllocator;

    std::mutex mQueueLock;
    std::condition_variable mQueueCondition;
    std::list<std::unique_ptr<C2Work>> mWorkQueue;

    std::shared_ptr<android::Codec2Client> mClient;
    std::shared_ptr<android::Codec2Client::Listener> mListener;
    std::shared_ptr<android::Codec2Client::Component> mComponent;

   protected:
    static void description(const std::string& description) {
        RecordProperty("description", description);
    }
};

void validateComponent(
    const std::shared_ptr<android::Codec2Client::Component>& component,
    Codec2AudioEncHidlTest::standardComp compName, bool& disableTest) {
    // Validate its a C2 Component
    if (component->getName().find("c2") == std::string::npos) {
        ALOGE("Not a c2 component");
        disableTest = true;
        return;
    }

    // Validate its not an encoder and the component to be tested is audio
    if (component->getName().find("decoder") != std::string::npos) {
        ALOGE("Expected Encoder, given Decoder");
        disableTest = true;
        return;
    }
    std::vector<std::unique_ptr<C2Param>> queried;
    c2_status_t c2err =
        component->query({}, {C2PortMediaTypeSetting::input::PARAM_TYPE},
                         C2_DONT_BLOCK, &queried);
    if (c2err != C2_OK && queried.size() == 0) {
        ALOGE("Query media type failed => %d", c2err);
    } else {
        std::string inputDomain =
            ((C2StreamMediaTypeSetting::input*)queried[0].get())->m.value;
        if (inputDomain.find("audio/") == std::string::npos) {
            ALOGE("Expected Audio Component");
            disableTest = true;
            return;
        }
    }

    // Validates component name
    if (compName == Codec2AudioEncHidlTest::unknown_comp) {
        ALOGE("Component InValid");
        disableTest = true;
        return;
    }
    ALOGV("Component Valid");
}

// Set Default config param.
void setupConfigParam(
    const std::shared_ptr<android::Codec2Client::Component>& component,
    int32_t nChannels, int32_t nSampleRate) {
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    C2StreamSampleRateInfo::input sampleRateInfo(0u, nSampleRate);
    C2StreamChannelCountInfo::input channelCountInfo(0u, nChannels);

    std::vector<C2Param*> configParam{&sampleRateInfo, &channelCountInfo};
    c2_status_t status =
        component->config(configParam, C2_DONT_BLOCK, &failures);
    ASSERT_EQ(failures.size(), 0u);
    ASSERT_EQ(status, C2_OK);
}

// LookUpTable of clips and metadata for component testing
void GetURLForComponent(Codec2AudioEncHidlTest::standardComp comp, char* mURL) {
    struct CompToURL {
        Codec2AudioEncHidlTest::standardComp comp;
        const char* mURL;
    };
    static const CompToURL kCompToURL[] = {
        {Codec2AudioEncHidlTest::standardComp::aac,
         "bbb_raw_2ch_48khz_s16le.raw"},
        {Codec2AudioEncHidlTest::standardComp::amrnb,
         "bbb_raw_1ch_8khz_s16le.raw"},
        {Codec2AudioEncHidlTest::standardComp::amrwb,
         "bbb_raw_1ch_16khz_s16le.raw"},
        {Codec2AudioEncHidlTest::standardComp::flac,
         "bbb_raw_2ch_48khz_s16le.raw"},
    };

    for (size_t i = 0; i < sizeof(kCompToURL) / sizeof(kCompToURL[0]); ++i) {
        if (kCompToURL[i].comp == comp) {
            strcat(mURL, kCompToURL[i].mURL);
            return;
        }
    }
}

void encodeNFrames(const std::shared_ptr<android::Codec2Client::Component> &component,
                   std::mutex &queueLock, std::condition_variable &queueCondition,
                   std::list<std::unique_ptr<C2Work>> &workQueue,
                   std::shared_ptr<C2Allocator> &linearAllocator,
                   std::shared_ptr<C2BlockPool> &linearPool,
                   C2BlockPool::local_id_t blockPoolId,
                   std::ifstream& eleStream, uint32_t nFrames,
                   int32_t samplesPerFrame, int32_t nChannels,
                   int32_t nSampleRate,bool signalEOS = true) {
    typedef std::unique_lock<std::mutex> ULock;
    linearPool =
        std::make_shared<C2PooledBlockPool>(linearAllocator, blockPoolId++);
    component->start();

    uint32_t frameID = 0;
    uint32_t maxRetry = 0;
    int bytesCount = samplesPerFrame * nChannels * 2;
    int32_t timestampIncr =
        (int)(((float)samplesPerFrame / nSampleRate) * 1000000);
    uint64_t timestamp = 0;
    while (1) {
        if (nFrames == 0) break;
        uint32_t flags = 0;
        std::unique_ptr<C2Work> work;
        // Prepare C2Work
        while (!work && (maxRetry < MAX_RETRY)) {
            ULock l(queueLock);
            if (!workQueue.empty()) {
                work.swap(workQueue.front());
                workQueue.pop_front();
            } else {
                queueCondition.wait_for(l, TIME_OUT);
                maxRetry++;
            }
        }
        if (!work && (maxRetry >= MAX_RETRY)) {
            ASSERT_TRUE(false) << "Wait for generating C2Work exceeded timeout";
        }
        if (signalEOS && (nFrames == 1))
            flags |= C2FrameData::FLAG_END_OF_STREAM;

        work->input.flags = (C2FrameData::flags_t)flags;
        work->input.ordinal.timestamp = timestamp;
        work->input.ordinal.frameIndex = frameID;
        char* data = (char*)malloc(bytesCount);
        eleStream.read(data, bytesCount);
        ASSERT_EQ(eleStream.gcount(), bytesCount);
        std::shared_ptr<C2LinearBlock> block;
        ASSERT_EQ(C2_OK, linearPool->fetchLinearBlock(
                             bytesCount, {C2MemoryUsage::CPU_READ,
                                          C2MemoryUsage::CPU_WRITE},
                             &block));
        ASSERT_TRUE(block);
        // Write View
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK) {
            fprintf(stderr, "C2LinearBlock::map() failed : %d", view.error());
            break;
        }
        ASSERT_EQ((size_t)bytesCount, view.capacity());
        ASSERT_EQ(0u, view.offset());
        ASSERT_EQ((size_t)bytesCount, view.size());

        memcpy(view.base(), data, bytesCount);
        work->input.buffers.clear();
        work->input.buffers.emplace_back(new LinearBuffer(block));
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);
        free(data);

        std::list<std::unique_ptr<C2Work>> items;
        items.push_back(std::move(work));

        // DO THE DECODING
        ASSERT_EQ(component->queue(&items), C2_OK);
        ALOGV("Frame #%d size = %d queued", frameID, bytesCount);
        nFrames--;
        timestamp += timestampIncr;
        frameID++;
        maxRetry = 0;
    }
}

void waitOnInputConsumption(std::mutex& queueLock,
                            std::condition_variable& queueCondition,
                            std::list<std::unique_ptr<C2Work>>& workQueue) {
    typedef std::unique_lock<std::mutex> ULock;
    uint32_t queueSize;
    uint32_t maxRetry = 0;
    {
        ULock l(queueLock);
        queueSize = workQueue.size();
    }
    while ((maxRetry < MAX_RETRY) && (queueSize < MAX_INPUT_BUFFERS)) {
        ULock l(queueLock);
        if (queueSize != workQueue.size()) {
            queueSize = workQueue.size();
            maxRetry = 0;
        } else {
            queueCondition.wait_for(l, TIME_OUT);
            maxRetry++;
        }
    }
}

TEST_F(Codec2AudioEncHidlTest, validateCompName) {
    if (mDisableTest) return;
    ALOGV("Checks if the given component is a valid audio component");
    validateComponent(mComponent, mCompName, mDisableTest);
    ASSERT_EQ(mDisableTest, false);
}

TEST_F(Codec2AudioEncHidlTest, EncodeTest) {
    ALOGV("EncodeTest");
    if (mDisableTest) return;
    char mURL[512];
    strcpy(mURL, gEnv->getRes().c_str());
    GetURLForComponent(mCompName, mURL);

    // Setting default configuration
    int32_t nChannels = 2;
    int32_t nSampleRate = 44100;
    int32_t samplesPerFrame = 1024;
    switch (mCompName) {
        case aac:
            nChannels = 2;
            nSampleRate = 48000;
            samplesPerFrame = 1024;
            break;
        case flac:
            nChannels = 2;
            nSampleRate = 48000;
            samplesPerFrame = 1152;
            break;
        case amrnb:
            nChannels = 1;
            nSampleRate = 8000;
            samplesPerFrame = 160;
            break;
        case amrwb:
            nChannels = 1;
            nSampleRate = 16000;
            samplesPerFrame = 160;
            break;
        default:
            ASSERT_TRUE(false);
    }
    setupConfigParam(mComponent, nChannels, nSampleRate);
    std::ifstream eleStream;
    eleStream.open(mURL, std::ifstream::binary);
    ASSERT_EQ(eleStream.is_open(), true);
    ALOGV("mURL : %s", mURL);
    ASSERT_NO_FATAL_FAILURE(
        encodeNFrames(mComponent, mQueueLock, mQueueCondition, mWorkQueue,
                      mLinearAllocator, mLinearPool, mBlockPoolId, eleStream,
                      128, samplesPerFrame, nChannels, nSampleRate));

    // blocking call to ensures application to Wait till all the inputs are
    // consumed
    if (!mEos) {
        ALOGD("Waiting for input consumption");
        ASSERT_NO_FATAL_FAILURE(
            waitOnInputConsumption(mQueueLock, mQueueCondition, mWorkQueue));
    }

    eleStream.close();
    if (mFramesReceived != 128) {
        ALOGE("Input buffer count and Output buffer count mismatch");
        ALOGE("framesReceived : %d inputFrames : 128", mFramesReceived);
        ASSERT_TRUE(false);
    }
    if ((mCompName == flac || mCompName == aac)) {
        if (!mCsd) {
            ALOGE("CSD buffer missing");
            ASSERT_TRUE(false);
        }
    }
}

}  // anonymous namespace

// TODO : Thumbnail Test
// TODO : Test EOS
// TODO : Flush Test
// TODO : Timestamps deviation
int main(int argc, char** argv) {
    gEnv = new ComponentTestEnvironment();
    ::testing::AddGlobalTestEnvironment(gEnv);
    ::testing::InitGoogleTest(&argc, argv);
    gEnv->init(&argc, argv);
    int status = gEnv->initFromOptions(argc, argv);
    if (status == 0) {
        int status = RUN_ALL_TESTS();
        LOG(INFO) << "C2 Test result = " << status;
    }
    return status;
}
