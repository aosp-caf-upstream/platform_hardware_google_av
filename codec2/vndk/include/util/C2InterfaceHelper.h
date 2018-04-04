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

#ifndef C2UTILS_INTERFACE_HELPER_H_
#define C2UTILS_INTERFACE_HELPER_H_

#include <C2Component.h>
#include <util/C2ParamUtils.h> // for _C2Tuple
#include <util/C2InterfaceUtils.h>

#include <map>
#include <vector>

#include <stddef.h>

/**
 * Interface Helper
 */
using C2R = C2SettingResultsBuilder;

template<typename T, bool E=std::is_enum<T>::value>
struct _c2_reduce_enum_to_underlying_type {
    typedef T type;
};

template<typename T>
struct _c2_reduce_enum_to_underlying_type<T, true> {
    typedef typename std::underlying_type<T>::type type;
};

/**
 * Helper class to implement parameter reflectors. This class is dynamic and is designed to be
 * shared by multiple interfaces. This allows interfaces to add structure descriptors as needed.
 */
class C2ReflectorHelper : public C2ParamReflector {
public:
    C2ReflectorHelper() = default;
    virtual ~C2ReflectorHelper() = default;
    virtual std::unique_ptr<C2StructDescriptor> describe(
            C2Param::CoreIndex paramIndex) const override;

    /**
     * Adds support for describing the given parameters.
     *
     * \param Params types of codec 2.0 structs (or parameters) to describe
     */
    template<typename... Params>
    C2_INLINE void addStructDescriptors() {
        std::vector<C2StructDescriptor> structs;
        addStructDescriptors(structs, (_C2Tuple<Params...> *)nullptr);
    }

    /**
     * Adds support for describing a specific struct.
     *
     * \param strukt descriptor for the struct that will be moved out.
     */
    void addStructDescriptor(C2StructDescriptor &&strukt);

private:
    /**
     * Adds support for describing the given descriptors.
     *
     * \param structs List of structure descriptors to add support for
     */
    C2_HIDE void addStructDescriptors(
            std::vector<C2StructDescriptor> &structs, _C2Tuple<> *);

    /**
     * Utility method that adds support for describing the given descriptors in a recursive manner
     * one structure at a time using a list of structure descriptors temporary.
     *
     * \param T the type of codec 2.0 struct to describe
     * \param Params rest of the structs
     * \param structs Temporary list of structure descriptors used to optimize the operation.
     */
    template<typename T, typename... Params>
    C2_INLINE void addStructDescriptors(
            std::vector<C2StructDescriptor> &structs, _C2Tuple<T, Params...> *) {
        structs.emplace_back((T*)nullptr);
        addStructDescriptors((_C2Tuple<Params...> *)nullptr);
    }

    mutable std::mutex _mMutex;
    std::map<C2Param::CoreIndex, const C2StructDescriptor> _mStructs; ///< descriptors
};

/**
 * Utility class that implements the codec 2.0 interface API-s for some parameters.
 *
 * This class must be subclassed.
 */
class C2InterfaceHelper {
public:
    /**
     * Returns the base offset of a field at |offset| that could be part of an array or part of a
     * sub-structure.
     *
     * This method does not do field size verification, e.g. if offset if obtained from a structure,
     * it will not stop at the structure boundary - this is okay, as we just want the base offset
     * here, which is the same.
     */
    static
    size_t GetBaseOffset(const std::shared_ptr<C2ParamReflector> &reflector,
                                C2Param::CoreIndex index, size_t offset);

    /**
     * The interface helper class uses references to std::shared_ptr<T> config parameters.
     * Internally, these need to be generalized to std::shared_ptr<C2Param> refs, but the cast is
     * not allowed (as these are references). As such, this class never returns pointer to the
     * shared_ptrs.
     */
    struct ParamRef {
        template<typename T, typename enable=
                typename std::enable_if<std::is_convertible<T, C2Param>::value>::type>
        inline C2_HIDE ParamRef(std::shared_ptr<T> &param)
            : _mRef(reinterpret_cast<std::shared_ptr<C2Param>*>(&param)) { }

        // used by comparison operator for containers
        operator std::shared_ptr<C2Param> *() const { return _mRef; }

        /**
         * Returns a shared pointer to the parameter.
         */
        std::shared_ptr<C2Param> get() const { return *_mRef; }

    private:
        std::shared_ptr<C2Param> *_mRef;
    };

    /**
     * Field helper.
     *
     * Contains additional information for the field: possible values, and currently supported
     * values.
     */
    class FieldHelper {
    public:
        /**
         * Creates helper for a field with given possible values.
         *
         * \param param parameter reference. The parameter does not have to be initialized at this
         *        point.
         * \param field field identifier
         * \param values possible values for the field
         */
        FieldHelper(const ParamRef &param, const _C2FieldId &field,
                    std::unique_ptr<C2FieldSupportedValues> &&values);

        /**
         * Creates a param-field identifier for this field. This method is called after the
         * underlying parameter has been initialized.
         *
         * \aram index
         *
         * @return C2ParamField
         */
        C2ParamField makeParamField(C2Param::Index index) const;

        /**
         * Sets the currently supported values for this field.
         *
         * \param values currently supported values that will be moved out
         */
        void setSupportedValues(std::unique_ptr<C2FieldSupportedValues> &&values);

        /**
         * Gets the currently supported values for this field. This defaults to the possible values
         * if currently supported values were never set.
         */
        const C2FieldSupportedValues *getSupportedValues() const;

        /**
         * Gets the possible values for this field.
         */
        const C2FieldSupportedValues *getPossibleValues() const;

    protected:
        // TODO: move to impl for safety
        ParamRef mParam;
        _C2FieldId mFieldId;
        std::unique_ptr<C2FieldSupportedValues> mPossible;
        std::unique_ptr<C2FieldSupportedValues> mSupported; ///< if different from possible
    };

    template<typename T>
    struct C2_HIDE Param;
    class ParamHelper;

    /**
     * Factory is an interface to get the parameter helpers from a std::shared_ptr<T> &.
     */
    class Factory {
        // \todo this may be already in ParamHelper
        virtual std::shared_ptr<C2ParamReflector> getReflector() const = 0;

        virtual std::shared_ptr<ParamHelper> getParamHelper(const ParamRef &param) const = 0;

    public:
        virtual ~Factory() = default;

        template<typename T>
        Param<T> get(std::shared_ptr<T> &param, std::shared_ptr<T> altValue = nullptr) const {
            return Param<T>(getParamHelper(ParamRef(param)),
                            altValue == nullptr ? param : altValue,
                            getReflector());
        }
    };

    /**
     * Typed field helper.
     */
    template<typename T>
    struct C2_HIDE Field {
        /**
         * Constructor.
         *
         * \param helper helper for this field
         * \param index  parameter index (this is needed as it is not available during parameter
         *        construction) \todo remove
         */
        Field(std::shared_ptr<FieldHelper> helper, C2Param::Index index);

        bool supportsAtAll(T value) const {
            return C2FieldSupportedValuesHelper<T>(*_mHelper->getPossibleValues()).supports(value);
        }

        bool supportsNow(T value) const {
            return C2FieldSupportedValuesHelper<T>(*_mHelper->getSupportedValues()).supports(value);
        }

        /**
         * Creates a conflict resolution suggestion builder for this field.
         */
        C2ParamFieldValuesBuilder<T> shouldBe() const;

        /**
         * Creates a currently supported values builder for this field. This is only supported
         * for non-const fields to disallow setting supported values for dependencies.
         */
        C2ParamFieldValuesBuilder<T> mustBe();

        // TODO
        C2R validatePossible(const T &value __unused) const {
            /// TODO
            return C2R::Ok();
        }

    private:
        std::shared_ptr<FieldHelper> _mHelper;
        C2ParamField _mField;
    };

    template<typename T>
    using C2P = Param<T>;

    template<typename T>
    class ParamBuilder;

    class ParamHelper {
    public:
        ParamHelper(ParamRef param, C2StringLiteral name, C2StructDescriptor &&);
        ParamHelper(ParamHelper &&);
        ~ParamHelper();

        template<typename T>
        static ParamBuilder<T> Build(std::shared_ptr<T> &param, C2StringLiteral name) {
            return ParamBuilder<T>(param, name);
        }

        /**
         * Finds a field descriptor.
         */
        std::shared_ptr<FieldHelper> findField(size_t baseOffs, size_t baseSize) const;

        const ParamRef ref() const;

        std::shared_ptr<C2Param> value();

        std::shared_ptr<const C2Param> value() const;

        /**
         * Performs a configuration change request for this parameter.
         *
         * \param value    the value that is being assigned to this parameter.
         *                 This could be pointing to the current value of the
         *                 parameter. This must not change.
         * \param mayBlock whether blocking is allowed
         * \param endValue the resulting value
         * \param f        parameter factory (to access dependencies)
         * \param failures vector of failures to append any failures from this
         *                 operation
         *
         * \retval C2_OK        configuration was successful
         * \retval C2_BAD_VALUE value is incorrect (TBD)
         * \retval C2_NO_MEMORY not enough memory to perform the assignment
         * \retval C2_TIMED_OUT configuration timed out
         * \retval C2_BLOCKING  configuration requires blocking to be allowed
         * \retval C2_CORRUPTED interface is corrupted
         */
        c2_status_t trySet(
                const C2Param *value, bool mayBlock,
                bool *changed,
                Factory &f,
                std::vector<std::unique_ptr<C2SettingResult>>* const failures);

        const std::vector<C2Param::Index> getDownDependencies() const;

        void addDownDependency(C2Param::Index index);

        const std::vector<ParamRef> getDependenciesAsRefs() const;

        // returns and moves out stored struct descriptor
        C2StructDescriptor retrieveStructDescriptor();

        C2Param::Index index() const;

        std::shared_ptr</* TODO const*/ C2ParamDescriptor> getDescriptor();

    protected:
        typedef C2ParamDescriptor::attrib_t attrib_t;
        attrib_t& attrib();

        void setDefaultValue(std::shared_ptr<C2Param> default_);

        void setSetter(std::function<C2R(const C2Param *, bool, bool *, Factory &)> setter);

        void setGetter(std::function<std::shared_ptr<C2Param>(bool)> getter);

        void setDependencies(std::vector<C2Param::Index> indices, std::vector<ParamRef> refs);

        void addFields(std::vector<C2ParamFieldValues> &&fields);

        std::shared_ptr<ParamHelper> build();

        class Impl;
        std::unique_ptr<Impl> mImpl;
    };

    /**
     * Typed parameter helper. This provides access to members as well as field helpers.
     */
    template<typename T>
    struct C2_HIDE Param {
        Param(
                std::shared_ptr<ParamHelper> helper, std::shared_ptr<T> &param,
                std::shared_ptr<C2ParamReflector> reflector)
            : v(*param.get()),
              _mTypedParam(param),
              _mHelper(helper),
              _mReflector(reflector) { }

        template<typename S>
        using FieldType = Field<typename _c2_reduce_enum_to_underlying_type<typename std::remove_const<typename std::remove_extent<S>::type>::type>::type>;

        template<typename S>
        FieldType<S> F(S &field) {
            size_t offs = (uintptr_t)&field - (uintptr_t)&get();
            // this must fall either within sizeof(T) + FLEX_SIZE or param->size()
            // size_t size = sizeof(field);
            // mParam may be null
            size_t baseOffs = GetBaseOffset(_mReflector, T::CORE_INDEX, offs - sizeof(C2Param)) + sizeof(C2Param);
            size_t baseSize = sizeof(typename std::remove_extent<S>::type);

            std::shared_ptr<FieldHelper> helper = _mHelper->findField(baseOffs, baseSize);
            return FieldType<S>(helper, _mTypedParam->index());
        }

        // const Param have const Fields; however, remove const from S
        template<typename S>
        const FieldType<S> F(S &field) const {
            return const_cast<const FieldType<S>>(const_cast<Param *>(this)->F(field));
        }

        /// Returns a const ref value of this const param.
        const T &get() const {
            return *_mTypedParam.get();
        }

        /// Returns a modifiable ref value of this non-const param.
        T &set() {
            return *_mTypedParam.get();
        }

        /// Const-reference to the value.s
        T const &v;

    private:
        std::shared_ptr<T> _mTypedParam;
        std::shared_ptr<ParamHelper> _mHelper;
        std::shared_ptr<C2ParamReflector> _mReflector;
    };

    /**
     * Templated move builder class for a parameter helper.
     */
    template<typename T>
    class C2_HIDE ParamBuilder : private ParamHelper {
    public:
        /** Construct the parameter builder from minimal info required. */
        ParamBuilder(std::shared_ptr<T> &param, C2StringLiteral name)
            : ParamHelper(param, name, C2StructDescriptor((T*)nullptr)),
              mTypedParam(&param) {
            attrib() = attrib_t::IS_PERSISTENT;
        }

        /** Makes this parameter required. */
        inline ParamBuilder &required() {
            attrib() |= attrib_t::IS_REQUIRED;
            return *this;
        }

        /** Makes this parameter transient (not persistent). */
        inline ParamBuilder &transient() {
            attrib() &= ~attrib_t::IS_PERSISTENT;
            return *this;
        }

        /** Makes this parameter hidden (not exposed in JAVA API). */
        inline ParamBuilder &hidden() {
            attrib() |= attrib_t::IS_HIDDEN;
            return *this;
        }

        /** Makes this parameter internal (not exposed to query/settings). */
        inline ParamBuilder &internal() {
            attrib() |= attrib_t::IS_INTERNAL;
            return *this;
        }

        /** Adds default value. Must be added exactly once. */
        inline ParamBuilder &withDefault(std::shared_ptr<T> default_) {
            // CHECK(!mDefaultValue);
            // WARN_IF(!default_); // could be nullptr if OOM
            // technically, this could be in the parent
            *mTypedParam = std::shared_ptr<T>(T::From(C2Param::Copy(*default_).release()));
            setDefaultValue(default_);
            std::shared_ptr<T> *typedParam = mTypedParam;
            setGetter([typedParam](bool) -> std::shared_ptr<C2Param> {
                return std::static_pointer_cast<C2Param>(*typedParam);
            });
            return *this;
        }

        /** Adds default value. Must be added exactly once. */
        inline ParamBuilder &withDefault(T *default_) {
            return withDefault(std::shared_ptr<T>(default_));
        }

        /** Adds all fields to this parameter with their possible values. */
        inline ParamBuilder &withFields(std::vector<C2ParamFieldValues> &&fields_) {
            addFields(std::move(fields_));
            return *this;
        }

        /**
         * Adds a constant value (also as default). Must be added exactly once.
         *
         * Const parameters by definition have no dependencies.
         */
        inline ParamBuilder &withConstValue(std::shared_ptr<T> default_) {
            attrib() |= attrib_t::IS_CONST;
            setSetter([default_](const C2Param *value, bool mayBlock __unused, bool *changed, Factory &f __unused) -> C2R {
                *changed = false;
                const T *typedValue = T::From(value);
                if (typedValue == nullptr) {
                    return C2R::Corrupted(); // TODO BadValue/Invalid. This should not happen here.
                }
                if (*typedValue != *default_) {
                    return C2R::Corrupted(); // TODO ReadOnly(*default_);
                }
                *changed = false;
                return C2R::Ok();
            });
            // TODO define currently supported values based on default value
            return withDefault(default_);
        }

        /** Adds constant value (also as default). Must be added exactly once. */
        inline ParamBuilder &withConstValue(T *default_) {
            return withConstValue(std::shared_ptr<T>(default_));
        }

        /**
         * Use a strict setter.
         *
         * \param fn   strict setter
         * \param deps dependencies (references)
         */
        template<typename ... Deps>
        inline ParamBuilder &withSetter(
                C2R (*fn)(bool, const C2P<T> &, C2P<T> &, const C2P<Deps> &...),
                std::shared_ptr<Deps>& ... deps) {
            attrib() |= attrib_t::IS_STRICT;
            std::shared_ptr<T> *typedParam = mTypedParam;
            setSetter([typedParam, fn, &deps...](const C2Param *value, bool mayBlock, bool *changed, Factory &f) -> C2R {
                *changed = false;
                const T *typedValue = T::From(value);
                if (typedValue == nullptr) {
                    return C2R::Corrupted(); // TODO BadValue/Invalid. This should not happen here.
                }
                // Do copy-on-change for parameters in this helper so change can be detected by
                // a change of the pointer. Do this by working on a proposed value.
                std::shared_ptr<T> proposedValue =
                    std::shared_ptr<T>(T::From(C2Param::Copy(*value).release()));
                if (proposedValue == nullptr) {
                    return C2R::NoMemory(value->index());
                }
                C2P<T> oldValue = f.get(*typedParam);
                // Get a parameter helper with value pointing to proposedValue
                C2P<T> helper = f.get(*typedParam, proposedValue);
                C2R result = fn(mayBlock, oldValue, helper, f.get(deps)...);

                // If value changed, copy result to current value
                if (helper.get() != *typedParam->get()) {
                    *typedParam = proposedValue;
                    *changed = true;
                }
                return result;
            });
            setDependencies(std::vector<C2Param::Index>{ deps->index()... },
                            std::vector<ParamRef>{ ParamRef(deps)... });
            return *this;
        }

        /**
         * Use a non-strict setter.
         *
         * \param fn   non-strict setter
         * \param deps dependencies (references)
         */
        template<typename ... Deps>
        inline ParamBuilder &withSetter(
                C2R (*fn)(bool, C2P<T> &, const C2P<Deps> &...), std::shared_ptr<Deps>& ... deps) {
            std::shared_ptr<T> *typedParam = mTypedParam;
            setSetter([typedParam, fn, &deps...](const C2Param *value, bool mayBlock, bool *changed, Factory &f) -> C2R {
                *changed = false;
                const T *typedValue = T::From(value);
                if (typedValue == nullptr) {
                    return C2R::Corrupted(); // TODO BadValue/Invalid. This should not happen here.
                }
                // Do copy-on-change for parameters in this helper so change can be detected by
                // a change of the pointer. Do this by working on a proposed value.
                std::shared_ptr<T> proposedValue =
                    std::shared_ptr<T>(T::From(C2Param::Copy(*value).release()));
                if (proposedValue == nullptr) {
                    return C2R::NoMemory(value->index());
                }
                // Get a parameter helper with value pointing to proposedValue
                C2P<T> helper = f.get(*typedParam, proposedValue);
                C2R result = fn(mayBlock, helper, f.get(deps)...);

                // If value changed, copy result to current value
                if (helper.get() != *typedParam->get()) {
                    *typedParam = proposedValue;
                    *changed = true;
                }
                return result;
            });
            setDependencies(std::vector<C2Param::Index>{ deps->index()... },
                            std::vector<ParamRef>{ ParamRef(deps)... });
            return *this;
        }

        /**
         * Marks this a calculated (read-only) field.
         *
         * \param fn   non-strict setter (calculator)
         * \param deps dependencies (references)
         */
        template<typename ... Deps>
        inline ParamBuilder &calculatedAs(
                C2R (*fn)(bool, C2P<T> &, const C2P<Deps> &...), std::shared_ptr<Deps>& ... deps) {
            attrib() |= attrib_t::IS_READ_ONLY;
            return withSetter(fn, std::forward(deps...));
        }

        inline std::shared_ptr<ParamHelper> build() {
            return ParamHelper::build();
        }

    protected:
        std::shared_ptr<T> *mTypedParam;
    };

public:
    c2_status_t query(
            const std::vector<C2Param*> &stackParams,
            const std::vector<C2Param::Index> &heapParamIndices,
            c2_blocking_t mayBlock,
            std::vector<std::unique_ptr<C2Param>>* const heapParams) const;

    c2_status_t config(
           const std::vector<C2Param*> &params, c2_blocking_t mayBlock,
           std::vector<std::unique_ptr<C2SettingResult>>* const failures,
           std::vector<std::shared_ptr<C2Param>> *changes = nullptr);

    c2_status_t querySupportedParams(std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const {
        return _mFactory.querySupportedParams(params);
    }

    c2_status_t querySupportedValues(
            std::vector<C2FieldSupportedValuesQuery> &fields, c2_blocking_t mayBlock) const;

    std::shared_ptr<C2ReflectorHelper> getReflector() {
        return mReflector;
    }

private:
    void setInterfaceAddressBounds(uintptr_t start, uintptr_t end) {
        // TODO: exclude this helper
        (void)start;
        (void)end;
    }

protected:
    struct MyFactory : public Factory {
        virtual std::shared_ptr<C2ParamReflector> getReflector() const override {
            return _mReflector;
        }

        virtual std::shared_ptr<ParamHelper> getParamHelper(const ParamRef &param) const override {
            return _mParams.find(param)->second;
        }

    public:
        MyFactory(std::shared_ptr<C2ParamReflector> reflector)
            : _mReflector(reflector) { }

        virtual ~MyFactory() = default;

        void addParam(std::shared_ptr<ParamHelper> param) {
            _mParams.insert({param->ref(), param});
            _mIndexToHelper.insert({param->index(), param});

            // add down-dependencies
            for (const ParamRef &ref : param->getDependenciesAsRefs()) {
                // dependencies must already be defined
                _mParams.find(ref)->second->addDownDependency(param->index());
            }

            _mDependencyIndex.emplace(param->index(), _mDependencyIndex.size());
        }

        std::shared_ptr<ParamHelper> getParam(C2Param::Index ix) const {
            // TODO: handle streams separately
            const auto it = _mIndexToHelper.find(ix);
            if (it == _mIndexToHelper.end()) {
                return nullptr;
            }
            return it->second;
        }

        /**
         * TODO: this could return a copy using proper pointer cast.
         */
        std::shared_ptr<C2Param> getParamValue(C2Param::Index ix) const {
            std::shared_ptr<ParamHelper> helper = getParam(ix);
            return helper ? helper->value() : nullptr;
        }

        c2_status_t querySupportedParams(
                std::vector<std::shared_ptr<C2ParamDescriptor>> *const params) const {
            for (const auto &it : _mParams) {
                params->push_back(it.second->getDescriptor());
            }
            // TODO: handle errors
            return C2_OK;
        }

        size_t getDependencyIndex(C2Param::Index ix) {
            // in this version of the helper there is only a single stream so
            // we can look up directly by index
            auto it = _mDependencyIndex.find(ix);
            return it == _mDependencyIndex.end() ? SIZE_MAX : it->second;
        }

    private:
        std::map<ParamRef, std::shared_ptr<ParamHelper>> _mParams;
        std::map<C2Param::Index, std::shared_ptr<ParamHelper>> _mIndexToHelper;
        std::shared_ptr<C2ParamReflector> _mReflector;
        std::map<C2Param::Index, size_t> _mDependencyIndex;
    };

    std::shared_ptr<C2ReflectorHelper> mReflector;
    MyFactory _mFactory;

    void addParameter(std::shared_ptr<ParamHelper> param) {
        _mFactory.addParam(param);
        mReflector->addStructDescriptor(param->retrieveStructDescriptor());
    }

    size_t getDependencyIndex(C2Param::Index ix) {
        return _mFactory.getDependencyIndex(ix);
    }

    C2InterfaceHelper(std::shared_ptr<C2ReflectorHelper> reflector)
        : mReflector(reflector),
          _mFactory(reflector) { }

    /**
     * Sets subclass instance's address and size.
     *
     * \todo allow subclass to specify parameter address range directly (e.g. do not assume that
     *       they are local to the subclass instance)
     *
     * @param T type of the derived instance
     * @param instance pointer to the derived instance
     */
    template<typename T>
    inline C2_HIDE void setDerivedInstance(T *instance) {
        setInterfaceAddressBounds((uintptr_t)instance, (uintptr_t)(instance + 1));
    }

    C2_DO_NOT_COPY(C2InterfaceHelper);
};

/**
 * Creates a C2ParamFieldValuesBuilder class for a field of a parameter
 *
 * \param spParam a configuration parameter in an interface class subclassed from C2InterfaceHelper.
 * \param field   a field of such parameter
 */
#define C2F(spParam, field) \
    C2ParamFieldValuesBuilder< \
            typename _c2_reduce_enum_to_underlying_type< \
                    typename std::remove_extent<decltype(spParam->field)>::type>::type>( \
                            C2ParamField(spParam.get(), &spParam->field))

#endif  // C2UTILS_INTERFACE_HELPER_H_
