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

#ifndef HARDWARE_GOOGLE_MEDIA_C2_V1_0_UTILS_COMPONENT_H
#define HARDWARE_GOOGLE_MEDIA_C2_V1_0_UTILS_COMPONENT_H

#include <codec2/hidl/1.0/Configurable.h>

#include <hardware/google/media/c2/1.0/IComponentListener.h>
#include <hardware/google/media/c2/1.0/IComponentStore.h>
#include <hardware/google/media/c2/1.0/IComponent.h>
#include <hidl/MQDescriptor.h>
#include <hidl/Status.h>
#include <hwbinder/IBinder.h>

#include <C2Component.h>
#include <C2.h>

#include <map>

namespace hardware {
namespace google {
namespace media {
namespace c2 {
namespace V1_0 {
namespace utils {

using ::android::hardware::hidl_array;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::IBinder;
using ::android::sp;
using ::android::wp;

struct ComponentStore;

struct ComponentInterface : public Configurable<IComponentInterface> {
    ComponentInterface(
            const std::shared_ptr<C2ComponentInterface>& interface,
            const sp<ComponentStore>& store);
    c2_status_t status() const;

protected:
    c2_status_t mInit;
    std::shared_ptr<C2ComponentInterface> mInterface;
    sp<ComponentStore> mStore;
};

struct Component : public Configurable<IComponent> {
    Component(
            const std::shared_ptr<C2Component>&,
            const sp<IComponentListener>& listener,
            const sp<ComponentStore>& store);

    // Methods from gIComponent follow.
    virtual Return<Status> queue(const WorkBundle& workBundle) override;
    virtual Return<void> flush(flush_cb _hidl_cb) override;
    virtual Return<Status> drain(bool withEos) override;
    virtual Return<Status> connectToInputSurface(
            const sp<IInputSurface>& surface) override;
    virtual Return<Status> connectToOmxInputSurface(
            const sp<::android::hardware::graphics::bufferqueue::V1_0::
            IGraphicBufferProducer>& producer,
            const sp<::android::hardware::media::omx::V1_0::
            IGraphicBufferSource>& source) override;
    virtual Return<Status> disconnectFromInputSurface() override;
    virtual Return<void> createBlockPool(
            uint32_t allocatorId,
            createBlockPool_cb _hidl_cb) override;
    virtual Return<Status> start() override;
    virtual Return<Status> stop() override;
    virtual Return<Status> reset() override;
    virtual Return<Status> release() override;

protected:
    c2_status_t mInit;
    std::shared_ptr<C2Component> mComponent;
    std::shared_ptr<C2ComponentInterface> mInterface;
    sp<IComponentListener> mListener;
    sp<ComponentStore> mStore;

    struct ComparePointer {
        constexpr bool operator()(
                const wp<IBinder>& x, const wp<IBinder>& y) const {
            return std::less<IBinder*>()(x.unsafe_get(), y.unsafe_get());
        }
    };

    // Component lifetime management
    typedef std::map<wp<IBinder>, std::weak_ptr<C2Component>,
            ComparePointer> Roster;
    typedef Roster::const_iterator LocalId;
    LocalId mLocalId;

    void setLocalId(const LocalId& localId);
    virtual ~Component() override;

    friend struct ComponentStore;
};

}  // namespace utils
}  // namespace V1_0
}  // namespace c2
}  // namespace media
}  // namespace google
}  // namespace hardware

#endif  // HARDWARE_GOOGLE_MEDIA_C2_V1_0_UTILS_COMPONENT_H
