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

//#define LOG_NDEBUG 0
#define LOG_TAG "hardware.google.media.c2@1.0-service-software"

#include <C2PlatformSupport.h>

#include <codec2/hidl/1.0/ComponentStore.h>
#include <hidl/HidlTransportSupport.h>
#include <minijail.h>

// This is created by module "codec2.software.base.policy". This can be modified.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/codec2.software.base.policy";

// Additional device-specific seccomp permissions can be added in this file.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/codec2.software.ext.policy";

int main(int /* argc */, char** /* argv */) {
    ALOGD("hardware.google.media.c2@1.0-service-software starting...");

    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    // Extra threads may be needed to handle a stacked IPC sequence that
    // contains alternating binder and hwbinder calls. (See b/35283480.)
    android::hardware::configureRpcThreadpool(8, true /* callerWillJoin */);

    // Create IComponentStore service.
    {
        using namespace ::hardware::google::media::c2::V1_0;
        android::sp<IComponentStore> store =
                new utils::ComponentStore(
                android::GetCodec2PlatformComponentStore());
        if (store == nullptr) {
            ALOGE("Cannot create Codec2's IComponentStore software service.");
        } else {
            if (store->registerAsService("software") != android::OK) {
                ALOGE("Cannot register Codec2's "
                        "IComponentStore software service.");
            } else {
                ALOGI("Codec2's IComponentStore software service created.");
            }
        }

    }

    android::hardware::joinRpcThreadpool();
    return 0;
}

