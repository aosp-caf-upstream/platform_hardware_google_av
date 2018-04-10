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

#ifndef REFLECTED_PARAM_BUILDER_H_
#define REFLECTED_PARAM_BUILDER_H_

#include <map>
#include <memory>

#include <C2.h>
#include <C2Param.h>

#include <media/stagefright/foundation/AMessage.h>

namespace android {

/**
 * Build params by field name and values.
 */
class ReflectedParamUpdater {
public:
    ReflectedParamUpdater() = default;
    ~ReflectedParamUpdater() = default;

    /**
     * Add param descriptors so that this object can recognize the param and its
     * fields.
     *
     * \param reflector   C2ParamReflector object for C2Param reflection.
     * \param paramDescs  vector of C2ParamDescriptor objects that this object
     *                    would recognize when building params.
     */
    void addParamDesc(
            const std::shared_ptr<C2ParamReflector> &reflector,
            const std::vector<std::shared_ptr<C2ParamDescriptor>> &paramDescs);

    /**
     * Get list of param indices from field names in AMessage object.
     *
     * \param params[in]  AMessage object with field name to value pairs.
     * \param vec[out]    vector to store the indices from |params|.
     */
    void getParamIndicesFromMessage(
            const sp<AMessage> &params,
            std::vector<C2Param::Index> *vec /* nonnull */) const;

    /**
     * Update C2Param objects from field name and value in AMessage object.
     *
     * \param params[in]    AMessage object with field name to value pairs.
     * \param vec[in,out]   vector of the C2Param objects to be updated.
     */
    void updateParamsFromMessage(
            const sp<AMessage> &params,
            std::vector<std::unique_ptr<C2Param>> *vec /* nonnull */) const;

    /**
     * Clear param descriptors in this object.
     */
    void clear();

private:
    struct FieldDesc {
        std::shared_ptr<C2ParamDescriptor> paramDesc;
        std::unique_ptr<C2FieldDescriptor> fieldDesc;
        size_t offset;
    };
    std::map<std::string, FieldDesc> mMap;

    void parseMessageAndDoWork(
            const sp<AMessage> &params,
            std::function<void(const std::string &, const FieldDesc &, const void *, size_t)> work) const;

    C2_DO_NOT_COPY(ReflectedParamUpdater);
};

}  // namespace android

#endif  // REFLECTED_PARAM_BUILDER_H_
