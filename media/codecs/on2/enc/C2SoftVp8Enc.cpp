/*
 * Copyright 2018 The Android Open Source Project
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
#define LOG_TAG "C2SoftVp8Enc"
#include <utils/Log.h>
#include <utils/misc.h>

#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/MediaDefs.h>

#include "C2SoftVp8Enc.h"

namespace android {

constexpr char kComponentName[] = "c2.google.vp8.encoder";

std::shared_ptr<C2ComponentInterface> BuildIntf(
        const char *name, c2_node_id_t id,
        std::function<void(C2ComponentInterface*)> deleter =
            std::default_delete<C2ComponentInterface>()) {
    return SimpleC2Interface::Builder(name, id, deleter)
            .inputFormat(C2FormatVideo)
            .outputFormat(C2FormatCompressed)
            .inputMediaType(MEDIA_MIMETYPE_VIDEO_RAW)
            .outputMediaType(MEDIA_MIMETYPE_VIDEO_VP8)
            .build();
}

C2SoftVp8Enc::C2SoftVp8Enc(const char *name, c2_node_id_t id)
    : C2SoftVpxEnc(BuildIntf(name, id)),
      mDCTPartitions(0),
      mProfile(1) {
}

void C2SoftVp8Enc::setCodecSpecificInterface() {
    mCodecInterface = vpx_codec_vp8_cx();
}

void C2SoftVp8Enc::setCodecSpecificConfiguration() {
    switch (mProfile) {
        case 1:
            mCodecConfiguration->g_profile = 0;
            break;

        case 2:
            mCodecConfiguration->g_profile = 1;
            break;

        case 4:
            mCodecConfiguration->g_profile = 2;
            break;

        case 8:
            mCodecConfiguration->g_profile = 3;
            break;

        default:
            mCodecConfiguration->g_profile = 0;
    }
}

vpx_codec_err_t C2SoftVp8Enc::setCodecSpecificControls() {
    vpx_codec_err_t codec_return = vpx_codec_control(mCodecContext,
                                                     VP8E_SET_TOKEN_PARTITIONS,
                                                     mDCTPartitions);
    if (codec_return != VPX_CODEC_OK) {
        ALOGE("Error setting dct partitions for vpx encoder.");
    }
    return codec_return;
}

class C2SoftVp8EncFactory : public C2ComponentFactory {
public:
    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(new C2SoftVp8Enc(kComponentName, id), deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = BuildIntf(kComponentName, id, deleter);
        return C2_OK;
    }

    virtual ~C2SoftVp8EncFactory() override = default;
};

}  // namespace android

extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2SoftVp8EncFactory();
}

extern "C" void DestroyCodec2Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}

