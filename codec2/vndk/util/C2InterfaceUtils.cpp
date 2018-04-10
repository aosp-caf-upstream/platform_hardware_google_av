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

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-value"

#define C2_LOG_VERBOSE

#include <C2Debug.h>
#include <C2Param.h>
#include <C2ParamDef.h>
#include <C2ParamInternal.h>
#include <util/C2ParamUtils.h>
#include <util/C2InterfaceUtils.h>
#include <C2Work.h>
#include <C2Component.h>

#include <cmath>
#include <limits>
#include <map>
#include <type_traits>

#include <android-base/stringprintf.h>

std::ostream& operator<<(std::ostream& os, const _C2FieldId &i);

std::ostream& operator<<(std::ostream& os, const C2ParamField &i);

/* ---------------------------- C2SupportedRange ---------------------------- */

/**
 * Helper class for supported values range calculations.
 */
template<typename T, bool FP=std::is_floating_point<T>::value>
struct _C2TypedSupportedRangeHelper {
    /**
     * type of range size: a - b if a >= b and a and b are of type T
     */
    typedef typename std::make_unsigned<T>::type DiffType;

    /**
     * calculate (high - low) mod step
     */
    static DiffType mod(T low, T high, T step) {
        return DiffType(high - low) % DiffType(step);
    }
};

template<typename T>
struct _C2TypedSupportedRangeHelper<T, true> {
    typedef T DiffType;

    static DiffType mod(T low, T high, T step) {
        return fmod(high - low, step);
    }
};

template<typename T>
C2SupportedRange<T>::C2SupportedRange(const C2FieldSupportedValues &values) {
    if (values.type == C2FieldSupportedValues::RANGE) {
        _mMin = values.range.min.ref<ValueType>();
        _mMax = values.range.max.ref<ValueType>();
        _mStep = values.range.step.ref<ValueType>();
        _mNum = values.range.num.ref<ValueType>();
        _mDenom = values.range.denom.ref<ValueType>();
    } else {
        _mMin = MAX_VALUE;
        _mMax = MIN_VALUE;
        _mStep = MIN_STEP;
        _mNum = 0;
        _mDenom = 0;
    }
}

template<typename T>
bool C2SupportedRange<T>::contains(T value) const {
    // value must fall between min and max
    if (value < _mMin || value > _mMax) {
        return false;
    }
    // simple ranges contain all values between min and max
    if (isSimpleRange()) {
        return true;
    }
    // min is always part of the range
    if (value == _mMin) {
        return true;
    }
    // stepped ranges require (val - min) % step to be zero
    if (isArithmeticSeries()) {
        return _C2TypedSupportedRangeHelper<T>::mod(_mMin, value, _mStep) == 0;
    }
    // pure geometric series require (val / min) to be integer multiple of (num/denom)
    if (isGeometricSeries()) {
        if (value <= 0) {
            return false;
        }
        double log2base = log2(_mNum / _mDenom);
        double power = llround(log2(value / double(_mMin)) / log2base);
        // TODO: validate that result falls within precision (other than round)
        return value == T(_mMin * pow(_mNum / _mDenom, power) + MIN_STEP / 2);
    }
    // multiply-accumulate series require validating by walking through the series
    if (isMacSeries()) {
        double lastValue = _mMin;
        double base = _mNum / _mDenom;
        while (true) {
            // this cast is safe as _mMin <= lastValue <= _mMax
            if (T(lastValue + MIN_STEP / 2) == value) {
                return true;
            }
            double nextValue = fma(lastValue, base, _mStep);
            if (nextValue <= lastValue || nextValue > _mMax) {
                return false; // series is no longer monotonic or within range
            }
            lastValue = nextValue;
        };
    }
    // if we are here, this must be an invalid range
    return false;
}

template<typename T>
C2SupportedRange<T> C2SupportedRange<T>::limitedTo(const C2SupportedRange<T> &limit) const {
    // TODO - this only works for simple ranges
    return C2SupportedRange(std::max(_mMin, limit._mMin), std::min(_mMax, limit._mMax),
                                 std::max(_mStep, limit._mStep));
}

template class C2SupportedRange<uint8_t>;
template class C2SupportedRange<char>;
template class C2SupportedRange<int32_t>;
template class C2SupportedRange<uint32_t>;
//template class C2SupportedRange<c2_cntr32_t>;
template class C2SupportedRange<int64_t>;
template class C2SupportedRange<uint64_t>;
//template class C2SupportedRange<c2_cntr64_t>;
template class C2SupportedRange<float>;

/* -------------------------- C2SupportedValueSet -------------------------- */

/**
 * Ordered supported value set for a field of a given type.
 */
template<typename T>
bool C2SupportedValueSet<T>::contains(T value) const {
    return std::find(_mValues.cbegin(), _mValues.cend(), C2Value::Primitive(value)) != _mValues.cend();
}

template<typename T>
C2SupportedValueSet<T> C2SupportedValueSet<T>::limitedTo(const C2SupportedValueSet<T> &limit) const {
    std::vector<C2Value::Primitive> values = _mValues; // make a copy
    std::remove_if(values.begin(), values.end(), [&limit](const C2Value::Primitive &v) -> bool {
        return !limit.contains(v.ref<ValueType>()); });
    return C2SupportedValueSet(std::move(values));
}

template<typename T>
C2SupportedValueSet<T> C2SupportedValueSet<T>::limitedTo(const C2SupportedRange<T> &limit) const {
    std::vector<C2Value::Primitive> values = _mValues; // make a copy
    std::remove_if(values.begin(), values.end(), [&limit](const C2Value::Primitive &v) -> bool {
        return !limit.contains(v.ref<ValueType>()); });
    return C2SupportedValueSet(std::move(values));
}

template<typename T>
const std::vector<T> C2SupportedValueSet<T>::values() const {
    std::vector<T> vals(_mValues.size());
    std::transform(_mValues.cbegin(), _mValues.cend(), vals.begin(), [](const C2Value::Primitive &p) -> T {
        return p.ref<ValueType>();
    });
    return vals;
}

template class C2SupportedValueSet<uint8_t>;
template class C2SupportedValueSet<char>;
template class C2SupportedValueSet<int32_t>;
template class C2SupportedValueSet<uint32_t>;
//template class C2SupportedValueSet<c2_cntr32_t>;
template class C2SupportedValueSet<int64_t>;
template class C2SupportedValueSet<uint64_t>;
//template class C2SupportedValueSet<c2_cntr64_t>;
template class C2SupportedValueSet<float>;

/* ---------------------- C2FieldSupportedValuesHelper ---------------------- */

template<typename T>
struct C2FieldSupportedValuesHelper<T>::Impl {
    Impl(const C2FieldSupportedValues &values)
        : _mType(values.type),
          _mRange(values),
          _mValues(values) { }

    bool supports(T value) const;

private:
    typedef typename _C2FieldValueHelper<T>::ValueType ValueType;
    C2FieldSupportedValues::type_t _mType;
    C2SupportedRange<ValueType> _mRange;
    C2SupportedValueSet<ValueType> _mValues;

//    friend std::ostream& operator<< <T>(std::ostream& os, const C2FieldSupportedValuesHelper<T>::Impl &i);
//    friend std::ostream& operator<<(std::ostream& os, const Impl &i);
    std::ostream& streamOut(std::ostream& os) const;
};

template<typename T>
bool C2FieldSupportedValuesHelper<T>::Impl::supports(T value) const {
    switch (_mType) {
        case C2FieldSupportedValues::RANGE: return _mRange.contains(value);
        case C2FieldSupportedValues::VALUES: return _mValues.contains(value);
        default: return false;
    }
}

template<typename T>
C2FieldSupportedValuesHelper<T>::C2FieldSupportedValuesHelper(const C2FieldSupportedValues &values)
    : _mImpl(std::make_unique<C2FieldSupportedValuesHelper<T>::Impl>(values)) { }

template<typename T>
C2FieldSupportedValuesHelper<T>::~C2FieldSupportedValuesHelper() = default;

template<typename T>
bool C2FieldSupportedValuesHelper<T>::supports(T value) const {
    return _mImpl->supports(value);
}

template class C2FieldSupportedValuesHelper<uint8_t>;
template class C2FieldSupportedValuesHelper<char>;
template class C2FieldSupportedValuesHelper<int32_t>;
template class C2FieldSupportedValuesHelper<uint32_t>;
//template class C2FieldSupportedValuesHelper<c2_cntr32_t>;
template class C2FieldSupportedValuesHelper<int64_t>;
template class C2FieldSupportedValuesHelper<uint64_t>;
//template class C2FieldSupportedValuesHelper<c2_cntr64_t>;
template class C2FieldSupportedValuesHelper<float>;

/* ----------------------- C2ParamFieldValuesBuilder ----------------------- */

template<typename T>
struct C2ParamFieldValuesBuilder<T>::Impl {
    Impl(const C2ParamField &field)
        : _mParamField(field),
          _mType(type_t::RANGE),
          _mDefined(false),
          _mRange(C2SupportedRange<T>::Any()),
          _mValues(C2SupportedValueSet<T>::None()) { }

    /**
     * Get C2ParamFieldValues from this builder.
     */
    operator C2ParamFieldValues() const {
        if (!_mDefined) {
            return C2ParamFieldValues(_mParamField);
        }
        switch (_mType) {
        case type_t::EMPTY:
        case type_t::VALUES:
            return C2ParamFieldValues(_mParamField, (C2FieldSupportedValues)_mValues);
        case type_t::RANGE:
            return C2ParamFieldValues(_mParamField, (C2FieldSupportedValues)_mRange);
        default:
            // TRESPASS
            // should never get here
            return C2ParamFieldValues(_mParamField);
        }
    }

    /** Define the supported values as the currently supported values of this builder. */
    void any() {
        _mDefined = true;
    }

    /** Restrict (and thus define) the supported values to none. */
    void none() {
        _mDefined = true;
        _mType = type_t::VALUES;
        _mValues.clear();
    }

    /** Restrict (and thus define) the supported values to |value| alone. */
    void equalTo(T value) {
         return limitTo(C2SupportedValueSet<T>::OneOf({value}));
    }

    /** Restrict (and thus define) the supported values to a value set. */
    void limitTo(const C2SupportedValueSet<T> &limit) {
        if (!_mDefined) {
            C2_LOG(VERBOSE) << "NA.limitTo(" << C2FieldSupportedValuesHelper<T>(limit) << ")";

            // shortcut for first limit applied
            _mDefined = true;
            _mValues = limit;
            _mType = _mValues.isEmpty() ? type_t::EMPTY : type_t::VALUES;
        } else {
            switch (_mType) {
            case type_t::EMPTY:
            case type_t::VALUES:
                C2_LOG(VERBOSE) << "(" << C2FieldSupportedValuesHelper<T>(_mValues) << ").limitTo("
                        << C2FieldSupportedValuesHelper<T>(limit) << ")";

                _mValues = _mValues.limitedTo(limit);
                _mType = _mValues.isEmpty() ? type_t::EMPTY : type_t::VALUES;
                break;
            case type_t::RANGE:
                C2_LOG(VERBOSE) << "(" << C2FieldSupportedValuesHelper<T>(_mRange) << ").limitTo("
                        << C2FieldSupportedValuesHelper<T>(limit) << ")";

                _mValues = limit.limitedTo(_mRange);
                _mType = _mValues.isEmpty() ? type_t::EMPTY : type_t::VALUES;
                break;
            default:
                C2_LOG(FATAL); // should not be here
            }
        }
        C2_LOG(VERBOSE) << " = " << _mType << ":" << C2FieldSupportedValuesHelper<T>(_mValues);
    }

    void limitTo(const C2SupportedRange<T> &limit) {
        if (!_mDefined) {
            C2_LOG(VERBOSE) << "NA.limitTo(" << C2FieldSupportedValuesHelper<T>(limit) << ")";

            // shortcut for first limit applied
            _mDefined = true;
            _mRange = limit;
            _mType = _mRange.isEmpty() ? type_t::EMPTY : type_t::RANGE;
            C2_LOG(VERBOSE) << " = " << _mType << ":" << C2FieldSupportedValuesHelper<T>(_mRange);
        } else {
            switch (_mType) {
            case type_t::EMPTY:
            case type_t::VALUES:
                C2_LOG(VERBOSE) << "(" << C2FieldSupportedValuesHelper<T>(_mValues) << ").limitTo("
                        << C2FieldSupportedValuesHelper<T>(limit) << ")";
                _mValues = _mValues.limitedTo(limit);
                _mType = _mValues.isEmpty() ? type_t::EMPTY : type_t::VALUES;
                C2_LOG(VERBOSE) << " = " << _mType << ":" << C2FieldSupportedValuesHelper<T>(_mValues);
                break;
            case type_t::RANGE:
                C2_LOG(VERBOSE) << "(" << C2FieldSupportedValuesHelper<T>(_mRange) << ").limitTo("
                        << C2FieldSupportedValuesHelper<T>(limit) << ")";
                _mRange = _mRange.limitedTo(limit);
                C2_DCHECK(_mValues.isEmpty());
                _mType = _mRange.isEmpty() ? type_t::EMPTY : type_t::RANGE;
                C2_LOG(VERBOSE) << " = " << _mType << ":" << C2FieldSupportedValuesHelper<T>(_mRange);
                break;
            default:
                C2_LOG(FATAL); // should not be here
            }
        }
    }

private:
    void instantiate() __unused {
        (void)_mValues.values(); // instantiate non-const values()
    }

    void instantiate() const __unused {
        (void)_mValues.values(); // instantiate const values()
    }

    typedef C2FieldSupportedValues::type_t type_t;

    C2ParamField _mParamField;
    type_t _mType;
    bool _mDefined;
    C2SupportedRange<T> _mRange;
    C2SupportedValueSet<T> _mValues;

};

template<typename T>
C2ParamFieldValuesBuilder<T>::operator C2ParamFieldValues() const {
    return (C2ParamFieldValues)(*_mImpl.get());
}

template<typename T>
C2ParamFieldValuesBuilder<T>::C2ParamFieldValuesBuilder(const C2ParamField &field)
    : _mImpl(std::make_unique<C2ParamFieldValuesBuilder<T>::Impl>(field)) { }

template<typename T>
C2ParamFieldValuesBuilder<T> &C2ParamFieldValuesBuilder<T>::any() {
    _mImpl->any();
    return *this;
}

template<typename T>
C2ParamFieldValuesBuilder<T> &C2ParamFieldValuesBuilder<T>::none() {
    _mImpl->none();
    return *this;
}

template<typename T>
C2ParamFieldValuesBuilder<T> &C2ParamFieldValuesBuilder<T>::equalTo(T value) {
    _mImpl->equalTo(value);
    return *this;
}

template<typename T>
C2ParamFieldValuesBuilder<T> &C2ParamFieldValuesBuilder<T>::limitTo(const C2SupportedValueSet<T> &limit) {
    _mImpl->limitTo(limit);
    return *this;
}

template<typename T>
C2ParamFieldValuesBuilder<T> &C2ParamFieldValuesBuilder<T>::limitTo(const C2SupportedRange<T> &limit) {
    _mImpl->limitTo(limit);
    return *this;
}

template<typename T>
C2ParamFieldValuesBuilder<T>::C2ParamFieldValuesBuilder(const C2ParamFieldValuesBuilder<T> &other)
    : _mImpl(std::make_unique<C2ParamFieldValuesBuilder<T>::Impl>(*other._mImpl.get())) { }

template<typename T>
C2ParamFieldValuesBuilder<T> &C2ParamFieldValuesBuilder<T>::operator=(
        const C2ParamFieldValuesBuilder<T> &other) {
    _mImpl = std::make_unique<C2ParamFieldValuesBuilder<T>::Impl>(*other._mImpl.get());
    return *this;
}

template<typename T>
C2ParamFieldValuesBuilder<T>::~C2ParamFieldValuesBuilder() = default;

template class C2ParamFieldValuesBuilder<uint8_t>;
template class C2ParamFieldValuesBuilder<char>;
template class C2ParamFieldValuesBuilder<int32_t>;
template class C2ParamFieldValuesBuilder<uint32_t>;
//template class C2ParamFieldValuesBuilder<c2_cntr32_t>;
template class C2ParamFieldValuesBuilder<int64_t>;
template class C2ParamFieldValuesBuilder<uint64_t>;
//template class C2ParamFieldValuesBuilder<c2_cntr64_t>;
template class C2ParamFieldValuesBuilder<float>;

/* ------------------------- C2SettingResultBuilder ------------------------- */

C2SettingConflictsBuilder::C2SettingConflictsBuilder() : _mConflicts() { }

C2SettingConflictsBuilder::C2SettingConflictsBuilder(C2ParamFieldValues &&conflict) {
    _mConflicts.emplace_back(std::move(conflict));
}

std::vector<C2ParamFieldValues> C2SettingConflictsBuilder::retrieveConflicts() {
    return std::move(_mConflicts);
}

/* ------------------------- C2SettingResult/sBuilder ------------------------- */

C2SettingResult C2SettingResultBuilder::ReadOnly(const C2ParamField &param) {
    return C2SettingResult { C2SettingResult::READ_ONLY, { param }, { } };
}

C2SettingResult C2SettingResultBuilder::BadValue(const C2ParamField &paramField) {
    return { C2SettingResult::BAD_VALUE, { paramField }, { } };
}

C2SettingResult C2SettingResultBuilder::Conflict(
        C2ParamFieldValues &&paramFieldValues, C2SettingConflictsBuilder &conflicts) {
    return C2SettingResult {
        C2SettingResult::CONFLICT, std::move(paramFieldValues), conflicts.retrieveConflicts()
    };
}

/*
C2SettingResultBuilder::C2SettingResultBuilder(const C2ParamField &paramField)
    : _mParamField(paramField),
      _mResult{ C2SettingResult::INFO_CONFLICT, { paramField }, { } } {
}
*/

C2SettingResultsBuilder::C2SettingResultsBuilder(C2SettingResult &&result)
        : _mStatus(C2_BAD_VALUE) {
    _mResults.emplace_back(new C2SettingResult(std::move(result)));
}

C2SettingResultsBuilder C2SettingResultsBuilder::plus(C2SettingResultsBuilder&& results) {
    for (std::unique_ptr<C2SettingResult> &r : results._mResults) {
        _mResults.emplace_back(std::move(r));
    }
    results._mResults.clear();
    // TODO: mStatus
    return std::move(*this);
}

c2_status_t C2SettingResultsBuilder::retrieveFailures(
        std::vector<std::unique_ptr<C2SettingResult>>* const failures) {
    for (std::unique_ptr<C2SettingResult> &r : _mResults) {
        failures->emplace_back(std::move(r));
    }
    _mResults.clear();
    return _mStatus;
}

C2SettingResultsBuilder::C2SettingResultsBuilder(c2_status_t status) : _mStatus(status) {
    // status must be one of OK, BAD_STATE, TIMED_OUT or CORRUPTED
    // mainly: BLOCKING, BAD_INDEX, BAD_VALUE and NO_MEMORY requires a setting attempt
}

#pragma clang diagnostic pop

