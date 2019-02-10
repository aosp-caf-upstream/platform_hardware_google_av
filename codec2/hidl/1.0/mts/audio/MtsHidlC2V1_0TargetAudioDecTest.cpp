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
#define LOG_TAG "codec2_hidl_hal_audio_dec_test"

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

struct FrameInfo {
    int bytesCount;
    uint32_t flags;
    int64_t timestamp;
};

class LinearBuffer : public C2Buffer {
   public:
    explicit LinearBuffer(const std::shared_ptr<C2LinearBlock>& block)
        : C2Buffer(
              {block->share(block->offset(), block->size(), ::C2Fence())}) {}
};

static ComponentTestEnvironment* gEnv = nullptr;

namespace {

class Codec2AudioDecHidlTest : public ::testing::VtsHalHidlTargetTestBase {
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
        ALOGV("Codec2AudioDecHidlTest SetUp");
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
            {"xaac", xaac},
            {"mp3", mp3},
            {"amrnb", amrnb},
            {"amrwb", amrwb},
            {"aac", aac},
            {"vorbis", vorbis},
            {"opus", opus},
            {"pcm", pcm},
            {"g711.alaw", g711alaw},
            {"g711.mlaw", g711mlaw},
            {"gsm", gsm},
            {"raw", raw},
            {"flac", flac},
        };
        const size_t kNumStringToName =
            sizeof(kStringToName) / sizeof(kStringToName[0]);

        std::string substring;
        std::string comp;
        substring = std::string(gEnv->getComponent());
        /* TODO: better approach to find the component */
        /* "c2.android." => 11th position */
        size_t pos = 11;
        size_t len = substring.find(".decoder", pos);
        comp = substring.substr(pos, len - pos);

        for (size_t i = 0; i < kNumStringToName; ++i) {
            if (!strcasecmp(comp.c_str(), kStringToName[i].Name)) {
                mCompName = kStringToName[i].CompName;
                break;
            }
        }
        mEos = false;
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
                    if ((param->index() == C2StreamSampleRateInfo::output::PARAM_TYPE) ||
                        (param->index() == C2StreamChannelCountInfo::output::PARAM_TYPE)) {
                        configParam.push_back(param);
                    }
                }
                mComponent->config(configParam, C2_DONT_BLOCK, &failures);
                ASSERT_EQ(failures.size(), 0u);
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
        xaac,
        mp3,
        amrnb,
        amrwb,
        aac,
        vorbis,
        opus,
        pcm,
        g711alaw,
        g711mlaw,
        gsm,
        raw,
        flac,
        unknown_comp,
    };

    bool mEos;
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
    Codec2AudioDecHidlTest::standardComp compName, bool& disableTest) {
    // Validate its a C2 Component
    if (component->getName().find("c2") == std::string::npos) {
        ALOGE("Not a c2 component");
        disableTest = true;
        return;
    }

    // Validate its not an encoder and the component to be tested is audio
    if (component->getName().find("encoder") != std::string::npos) {
        ALOGE("Expected Decoder, given Encoder");
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
    if (compName == Codec2AudioDecHidlTest::unknown_comp) {
        ALOGD("Component InValid");
        disableTest = true;
        return;
    }
    ALOGV("Component Valid");
}

// Set Default config param.
void setupConfigParam(
    const std::shared_ptr<android::Codec2Client::Component>& component,
    int32_t* bitStreamInfo) {
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    C2StreamSampleRateInfo::output sampleRateInfo(0u, bitStreamInfo[0]);
    C2StreamChannelCountInfo::output channelCountInfo(0u, bitStreamInfo[1]);

    std::vector<C2Param*> configParam{&sampleRateInfo, &channelCountInfo};
    c2_status_t status =
        component->config(configParam, C2_DONT_BLOCK, &failures);
    ASSERT_EQ(failures.size(), 0u);
    ASSERT_EQ(status, C2_OK);
}

// In decoder components, often the input parameters get updated upon
// parsing the header of elementary stream. Client needs to collect this
// information and reconfigure
void getInputChannelInfo(
    const std::shared_ptr<android::Codec2Client::Component>& component,
    Codec2AudioDecHidlTest::standardComp compName, int32_t* bitStreamInfo) {
    // query nSampleRate and nChannels
    std::initializer_list<C2Param::Index> indices{
        C2StreamSampleRateInfo::output::PARAM_TYPE,
        C2StreamChannelCountInfo::output::PARAM_TYPE,
    };
    std::vector<std::unique_ptr<C2Param>> inParams;
    c2_status_t status =
        component->query({}, indices, C2_DONT_BLOCK, &inParams);
    if (status != C2_OK && inParams.size() == 0) {
        ALOGE("Query media type failed => %d", status);
        ASSERT_TRUE(false);
    } else {
        size_t offset = sizeof(C2Param);
        ALOGD("--------------PARAMS-----------------");
        for (size_t i = 0; i < inParams.size(); ++i) {
            C2Param* param = inParams[i].get();
            bitStreamInfo[i] = *(int32_t*)((uint8_t*)param + offset);
            ALOGD("Param value : %u", *(int32_t*)((uint8_t*)param + offset));
            ALOGD("Param isVendor %d", param->isVendor());
            ALOGD("Param forInput %d", param->forInput());
            ALOGD("Param forOutput %d", param->forOutput());
            switch (param->kind()) {
                case C2Param::NONE:
                    ALOGD("Param kind NONE");
                    break;
                case C2Param::STRUCT:
                    ALOGD("Param kind STRUCT");
                    break;
                case C2Param::INFO:
                    ALOGD("Param kind INFO");
                    break;
                case C2Param::SETTING:
                    ALOGD("Param kind SETTING");
                    break;
                case C2Param::TUNING:
                    ALOGD("Param kind TUNING");
                    break;
                default:
                    ALOGD("Param kind Invalid");
                    break;
            }
            ALOGD("Param size (if size is 0 parameter is invalid) %zu",
                  param->size());
            ALOGD("--------------------------------------");
        }
        switch (compName) {
            case Codec2AudioDecHidlTest::amrnb: {
                ASSERT_EQ(bitStreamInfo[0], 8000);
                ASSERT_EQ(bitStreamInfo[1], 1);
                break;
            }
            case Codec2AudioDecHidlTest::amrwb: {
                ASSERT_EQ(bitStreamInfo[0], 16000);
                ASSERT_EQ(bitStreamInfo[1], 1);
                break;
            }
            case Codec2AudioDecHidlTest::gsm: {
                ASSERT_EQ(bitStreamInfo[0], 8000);
                break;
            }
            default:
                break;
        }
    }
}

// LookUpTable of clips and metadata for component testing
void GetURLForComponent(Codec2AudioDecHidlTest::standardComp comp, char* mURL,
                        char* info) {
    struct CompToURL {
        Codec2AudioDecHidlTest::standardComp comp;
        const char* mURL;
        const char* info;
    };
    static const CompToURL kCompToURL[] = {
        {Codec2AudioDecHidlTest::standardComp::xaac,
         "bbb_aac_stereo_128kbps_48000hz.aac",
         "bbb_aac_stereo_128kbps_48000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::mp3,
         "bbb_mp3_stereo_192kbps_48000hz.mp3",
         "bbb_mp3_stereo_192kbps_48000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::aac,
         "bbb_aac_stereo_128kbps_48000hz.aac",
         "bbb_aac_stereo_128kbps_48000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::amrnb,
         "sine_amrnb_1ch_12kbps_8000hz.amrnb",
         "sine_amrnb_1ch_12kbps_8000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::amrwb,
         "bbb_amrwb_1ch_14kbps_16000hz.amrwb",
         "bbb_amrwb_1ch_14kbps_16000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::vorbis,
         "bbb_vorbis_stereo_128kbps_48000hz.vorbis",
         "bbb_vorbis_stereo_128kbps_48000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::opus,
         "bbb_opus_stereo_128kbps_48000hz.opus",
         "bbb_opus_stereo_128kbps_48000hz.info"},
        {Codec2AudioDecHidlTest::standardComp::g711alaw,
        "bbb_g711alaw_1ch_8khz.raw",
         "bbb_g711alaw_1ch_8khz.info"},
        {Codec2AudioDecHidlTest::standardComp::g711mlaw,
        "bbb_g711mulaw_1ch_8khz.raw",
         "bbb_g711mulaw_1ch_8khz.info"},
        {Codec2AudioDecHidlTest::standardComp::gsm,
        "bbb_gsm_1ch_8khz_13kbps.raw",
         "bbb_gsm_1ch_8khz_13kbps.info"},
        {Codec2AudioDecHidlTest::standardComp::raw,
        "bbb_raw_1ch_8khz_s32le.raw",
         "bbb_raw_1ch_8khz_s32le.info"},
        {Codec2AudioDecHidlTest::standardComp::flac,
         "bbb_flac_stereo_680kbps_48000hz.flac",
         "bbb_flac_stereo_680kbps_48000hz.info"},
    };

    for (size_t i = 0; i < sizeof(kCompToURL) / sizeof(kCompToURL[0]); ++i) {
        if (kCompToURL[i].comp == comp) {
            strcat(mURL, kCompToURL[i].mURL);
            strcat(info, kCompToURL[i].info);
            return;
        }
    }
}

void decodeNFrames(const std::shared_ptr<android::Codec2Client::Component> &component,
                   std::mutex &queueLock, std::condition_variable &queueCondition,
                   std::list<std::unique_ptr<C2Work>> &workQueue,
                   std::shared_ptr<C2Allocator> &linearAllocator,
                   std::shared_ptr<C2BlockPool> &linearPool,
                   C2BlockPool::local_id_t blockPoolId,
                   std::ifstream& eleStream,
                   android::Vector<FrameInfo>* Info,
                   int offset, int range, bool signalEOS = true) {
    typedef std::unique_lock<std::mutex> ULock;
    int frameID = 0;
    linearPool =
        std::make_shared<C2PooledBlockPool>(linearAllocator, blockPoolId++);
    component->start();
    int maxRetry = 0;
    while (1) {
        if (frameID == (int)Info->size() || frameID == (offset + range)) break;
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
        int64_t timestamp = (*Info)[frameID].timestamp;
        if ((*Info)[frameID].flags) flags = 1u << ((*Info)[frameID].flags - 1);
        if (signalEOS && ((frameID == (int)Info->size() - 1) ||
                              (frameID == (offset + range - 1))))
                flags |= C2FrameData::FLAG_END_OF_STREAM;

        work->input.flags = (C2FrameData::flags_t)flags;
        work->input.ordinal.timestamp = timestamp;
        work->input.ordinal.frameIndex = frameID;
        int size = (*Info)[frameID].bytesCount;
        char* data = (char*)malloc(size);

        eleStream.read(data, size);
        ASSERT_EQ(eleStream.gcount(), size);

        std::shared_ptr<C2LinearBlock> block;
        ASSERT_EQ(C2_OK,
                  linearPool->fetchLinearBlock(
                      size, {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE},
                      &block));
        ASSERT_TRUE(block);

        // Write View
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK) {
            fprintf(stderr, "C2LinearBlock::map() failed : %d", view.error());
            break;
        }
        ASSERT_EQ((size_t)size, view.capacity());
        ASSERT_EQ(0u, view.offset());
        ASSERT_EQ((size_t)size, view.size());

        memcpy(view.base(), data, size);

        work->input.buffers.clear();
        work->input.buffers.emplace_back(new LinearBuffer(block));
        work->worklets.clear();
        work->worklets.emplace_back(new C2Worklet);
        free(data);

        std::list<std::unique_ptr<C2Work>> items;
        items.push_back(std::move(work));

        // DO THE DECODING
        ASSERT_EQ(component->queue(&items), C2_OK);
        ALOGV("Frame #%d size = %d queued", frameID, size);
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

TEST_F(Codec2AudioDecHidlTest, validateCompName) {
    if (mDisableTest) return;
    ALOGV("Checks if the given component is a valid audio component");
    validateComponent(mComponent, mCompName, mDisableTest);
    ASSERT_EQ(mDisableTest, false);
}

TEST_F(Codec2AudioDecHidlTest, configComp) {
    description("Tests component specific configuration");
    if (mDisableTest) return;
    int32_t bitStreamInfo[2] = {0};
    ASSERT_NO_FATAL_FAILURE(
        getInputChannelInfo(mComponent, mCompName, bitStreamInfo));
    setupConfigParam(mComponent, bitStreamInfo);
}

TEST_F(Codec2AudioDecHidlTest, DecodeTest) {
    description("Decodes input file");
    if (mDisableTest) return;

    char mURL[512], info[512];
    std::ifstream eleStream, eleInfo;

    strcpy(mURL, gEnv->getRes().c_str());
    strcpy(info, gEnv->getRes().c_str());
    GetURLForComponent(mCompName, mURL, info);

    eleInfo.open(info);
    ASSERT_EQ(eleInfo.is_open(), true);
    android::Vector<FrameInfo> Info;
    int bytesCount = 0;
    uint32_t flags = 0;
    uint32_t timestamp = 0;
    while (1) {
        if (!(eleInfo >> bytesCount)) break;
        eleInfo >> flags;
        eleInfo >> timestamp;
        Info.push_back({bytesCount, flags, timestamp});
    }
    eleInfo.close();
    int32_t bitStreamInfo[2] = {0};
    if (mCompName == raw) {
        bitStreamInfo[0] = 8000;
        bitStreamInfo[1] = 1;
    } else {
        ASSERT_NO_FATAL_FAILURE(
            getInputChannelInfo(mComponent, mCompName, bitStreamInfo));
    }
    setupConfigParam(mComponent, bitStreamInfo);
    ALOGV("mURL : %s", mURL);
    eleStream.open(mURL, std::ifstream::binary);
    ASSERT_EQ(eleStream.is_open(), true);
    ASSERT_NO_FATAL_FAILURE(decodeNFrames(
        mComponent, mQueueLock, mQueueCondition, mWorkQueue, mLinearAllocator,
        mLinearPool, mBlockPoolId, eleStream, &Info, 0, (int)Info.size()));

    // blocking call to ensures application to Wait till all the inputs are
    // consumed
    if (!mEos) {
        ALOGD("Waiting for input consumption");
        ASSERT_NO_FATAL_FAILURE(
            waitOnInputConsumption(mQueueLock, mQueueCondition, mWorkQueue));
    }

    eleStream.close();
    if (mFramesReceived != Info.size()) {
        ALOGE("Input buffer count and Output buffer count mismatch");
        ALOGE("framesReceived : %d inputFrames : %zu", mFramesReceived,
              Info.size());
        ASSERT_TRUE(false);
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
