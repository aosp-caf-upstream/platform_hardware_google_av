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

//#define LOG_NDEBUG 0
#define LOG_TAG "ReflectedParamUpdater"
#include <utils/Log.h>

#include <set>

#include <C2ParamInternal.h>

#include <media/stagefright/foundation/ADebug.h>

#include "ReflectedParamUpdater.h"

namespace android {

void ReflectedParamUpdater::addParamDesc(
        const std::shared_ptr<C2ParamReflector> &reflector,
        const std::vector<std::shared_ptr<C2ParamDescriptor>> &paramDescs) {
    for (const std::shared_ptr<C2ParamDescriptor> &desc : paramDescs) {
        C2String paramName = desc->name();

        // prefix vendor parameters
        if (desc->index().isVendor()) {
            paramName = "vendor." + paramName;
        }

        std::unique_ptr<C2StructDescriptor> structDesc = reflector->describe(
                desc->index().coreIndex());
        if (structDesc == nullptr) {
            ALOGD("Could not describe %s", paramName.c_str());
            continue;
        }
        for (auto it = structDesc->begin(); it != structDesc->end(); ++it) {
            if (it->type() & C2FieldDescriptor::STRUCT_FLAG) {
                // TODO: don't ignore
                ALOGD("ignored struct field");
                continue;
            }
            C2String fieldName = paramName + "." + it->name();
            switch (it->type()) {
                case C2FieldDescriptor::INT32:
                case C2FieldDescriptor::UINT32:
                case C2FieldDescriptor::CNTR32:
                case C2FieldDescriptor::INT64:
                case C2FieldDescriptor::UINT64:
                case C2FieldDescriptor::CNTR64:
                    if (it->extent() != 1) {
                        ALOGD("extent() != 1 for single value type: %s", fieldName.c_str());
                        fieldName.clear();
                        break;
                    }
                    break;
                case C2FieldDescriptor::STRING:
                    if (it->extent() == 0) {
                        ALOGD("extent() == 0 for string type: %s", fieldName.c_str());
                        fieldName.clear();
                        break;
                    }
                    // Don't use subscript for string
                    break;
                case C2FieldDescriptor::BLOB:
                    ALOGV("BLOB field not exposed: %s", fieldName.c_str());
                    fieldName.clear();
                    break;
                default:
                    ALOGD("Unrecognized type: %s", fieldName.c_str());
                    fieldName.clear();
                    break;
            }
            if (!fieldName.empty()) {
                ALOGV("%s registered", fieldName.c_str());
                // TODO: get the proper size by iterating through the fields.
                mMap[fieldName] = {
                    desc,
                    std::make_unique<C2FieldDescriptor>(
                            it->type(), it->extent(), it->name(),
                            _C2ParamInspector::GetOffset(*it),
                            _C2ParamInspector::GetSize(*it)),
                    0,  // offset
                };
            }
        }
    }
}

void ReflectedParamUpdater::getParamIndicesFromMessage(
        const sp<AMessage> &params,
        std::vector<C2Param::Index> *vec /* nonnull */) const {
    CHECK(vec != nullptr);
    vec->clear();
    std::set<C2Param::Index> indices;
    parseMessageAndDoWork(
            params,
            [&indices](const std::string &, const FieldDesc &desc, const void *, size_t) {
                indices.insert(desc.paramDesc->index());
            });
    for (const C2Param::Index &index : indices) {
        vec->push_back(index);
    }
}

void ReflectedParamUpdater::updateParamsFromMessage(
        const sp<AMessage> &params,
        std::vector<std::unique_ptr<C2Param>> *vec /* nonnull */) const {
    CHECK(vec != nullptr);

    std::map<C2Param::Index, C2Param *> paramsMap;
    for (const std::unique_ptr<C2Param> &param : *vec) {
        paramsMap[param->index()] = param.get();
    }

    parseMessageAndDoWork(
            params,
            [&paramsMap](const std::string &name, const FieldDesc &desc, const void *ptr, size_t size) {
                size_t offset = sizeof(C2Param) + desc.offset
                        + _C2ParamInspector::GetOffset(*desc.fieldDesc);
                C2Param *param = nullptr;
                auto paramIt = paramsMap.find(desc.paramDesc->index());
                if (paramIt == paramsMap.end()) {
                    ALOGD("%s found, but param #%d isn't present to update",
                            name.c_str(), (int32_t)desc.paramDesc->index());
                    return;
                }
                param = paramIt->second;
                memcpy((uint8_t *)param + offset, ptr, size);
            });
}

void ReflectedParamUpdater::parseMessageAndDoWork(
        const sp<AMessage> &params,
        std::function<void(const std::string &, const FieldDesc &, const void *, size_t)> work) const {
    for (const std::pair<const std::string, FieldDesc> &kv : mMap) {
        const std::string &name = kv.first;
        const FieldDesc &desc = kv.second;
        switch (desc.fieldDesc->type()) {
            case C2FieldDescriptor::INT32:
            case C2FieldDescriptor::UINT32:
            case C2FieldDescriptor::CNTR32: {
                int32_t tmp;
                if (!params->findInt32(name.c_str(), &tmp)) {
                    break;
                }
                work(name, desc, &tmp, sizeof(tmp));
                break;
            }

            case C2FieldDescriptor::INT64:
            case C2FieldDescriptor::UINT64:
            case C2FieldDescriptor::CNTR64: {
                int64_t tmp;
                if (!params->findInt64(name.c_str(), &tmp)) {
                    break;
                }
                work(name, desc, &tmp, sizeof(tmp));
                break;
            }

            case C2FieldDescriptor::FLOAT: {
                float tmp;
                if (!params->findFloat(name.c_str(), &tmp)) {
                    break;
                }
                work(name, desc, &tmp, sizeof(tmp));
                break;
            }

            case C2FieldDescriptor::STRING: {
                AString tmp;
                if (!params->findString(name.c_str(), &tmp)) {
                    break;
                }
                if (tmp.size() >= desc.fieldDesc->extent()) {
                    AString truncated(tmp, 0, desc.fieldDesc->extent() - 1);
                    ALOGD("String value too long to fit: original %s truncated %s",
                            tmp.c_str(), truncated.c_str());
                    tmp = truncated;
                }
                work(name, desc, tmp.c_str(), tmp.size() + 1);
                break;
            }

            case C2FieldDescriptor::BLOB:
            default:
                ALOGD("Unsupported data type for %s", name.c_str());
                // TODO: findBuffer?
                break;
        }
    }
}

void ReflectedParamUpdater::clear() {
    mMap.clear();
}

}  // namespace android
