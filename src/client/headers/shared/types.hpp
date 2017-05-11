﻿#pragma once
#include <stdio.h>
#include <set>
#include <array>
#include <mutex>
#include <thread>
#include <atomic>
#include <utility> //std::hash
#include "vector.hpp"
#include "pool.hpp"
#include <string_view>

namespace intercept {
    class sqf_functions;
    class registered_sqf_function_impl;
    namespace types {
        typedef uintptr_t(__cdecl *nular_function)(char *, uintptr_t);
        typedef uintptr_t(__cdecl *unary_function)(char *, uintptr_t, uintptr_t);
        typedef uintptr_t(__cdecl *binary_function)(char *, uintptr_t, uintptr_t, uintptr_t);

        typedef std::set<std::string> value_types;
        typedef uintptr_t value_type;
        class rv_pool_allocator;
        namespace __internal {
            void* rv_allocator_allocate_generic(size_t _size);
            void rv_allocator_deallocate_generic(void* _Ptr);
            void* rv_allocator_reallocate_generic(void* _Ptr, size_t _size);
            template <typename T, typename U>
            std::size_t pairhash(const T& first, const U& second) {
                size_t _hash = std::hash<T>()(first);
                _hash ^= std::hash<U>()(second) + 0x9e3779b9 + (_hash << 6) + (_hash >> 2);
                return _hash;
            }
        }

        template<class Type>
        class rv_allocator : std::allocator<Type> {
        public:
            static void deallocate(Type* _Ptr, size_t = 0) {
                return __internal::rv_allocator_deallocate_generic(_Ptr);
            }
            //This only allocates the memory! This will not be initialized to 0 and the allocated object will not have it's constructor called! 
            //use the create* Methods instead
            static Type* allocate(size_t _count) {
                return reinterpret_cast<Type*>(__internal::rv_allocator_allocate_generic(sizeof(Type)*_count));
            }

            //Allocates and Initializes one Object
            template<class... _Types>
            static Type* createSingle(_Types&&... _Args) {
                auto ptr = allocate(1);
                ::new (ptr) Type(std::forward<_Types>(_Args)...);
                //Have to do a little more unfancy stuff for people that want to overload the new operator
                return ptr;
            }

            //Allocates and initializes array of Elements using the default constructor.
            static Type* createArray(size_t _count) {
                if (_count == 1) return createSingle();

                auto ptr = allocate(_count);

                for (size_t i = 0; i < _count; ++i) {
                    ::new (ptr + i) Type();
                }

                return ptr;
            }

            static Type* createUninitializedArray(size_t _count) {
                if (_count == 1) return createSingle();

                auto ptr = allocate(_count);

                return ptr;
            }

            static Type* reallocate(Type* _Ptr, size_t _count) {
                return reinterpret_cast<Type*>(__internal::rv_allocator_reallocate_generic(_Ptr, sizeof(Type)*_count));
            }

            //#TODO implement game_data_pool and string pool here
        };

        class refcount_base {
        public:
            constexpr refcount_base() : _refcount(0) {}
            refcount_base(const refcount_base &src) { _refcount = 0; }
            void operator = (const refcount_base &src) const {}

            constexpr int add_ref() const {
                return ++_refcount;
            }
            int dec_ref() const {
                return --_refcount;
            }
            int ref_count() const {
                return _refcount;
            }
            mutable int _refcount;
        };

        //refcount has to be the first element in a class. Use refcount_vtable instead if refcount is preceded by a vtable
        class refcount : public refcount_base {
        public:
            virtual ~refcount() {}
            int release() const {
                int rcount = dec_ref();
                if (rcount == 0) {
                    //this->~refcount();
                    lastRefDeleted();
                    //rv_allocator<refcount>::deallocate(const_cast<refcount *>(this), 0);
                }
                return rcount;
            }
            void destruct() const {
                delete const_cast<refcount *>(this);
            }
            virtual void lastRefDeleted() const { destruct(); }
            virtual int __dummy_refcount_func() const { return 0; }
        };

        /*
        This is a placeholder so i can use refcount but still have an accessible vtable pointer
        */
        class __vtable {
        public:
            uintptr_t _vtable{ 0 };
        };

        template<class Type, class Allocator = rv_allocator<char>> //Has to be allocator of type char
        class compact_array : public refcount_base {
            static_assert(std::is_literal_type<Type>::value, "Type must be a literal type");
        public:

            int size() const { return _size; }
            Type *data() { return &_data; }
            const Type *data() const { return &_data; }


            //We delete ourselves! After release no one should have a pointer to us anymore!
            int release() const {
                int ret = dec_ref();
                if (!ret) del();
                return ret;
            }

            static compact_array* create(int number_of_elements_) {
                size_t size = sizeof(compact_array) + sizeof(Type)*(number_of_elements_ - 1);//-1 because we already have one element in compact_array
                compact_array* buffer = reinterpret_cast<compact_array*>(Allocator::allocate(size));
                new (buffer) compact_array(number_of_elements_);
                return buffer;
            }

            void del() const {
                this->~compact_array();
                const void* _thisptr = this;
                void* _thisptr2 = const_cast<void*>(_thisptr);
                Allocator::deallocate(reinterpret_cast<char*>(_thisptr2), _size - 1 + sizeof(compact_array));
            }
            //#TODO copy functions

            size_t _size;
            Type _data;
        private:
            explicit compact_array(size_t size_) {
                _size = size_;
            }
        };


        template<class Type>
        class ref {
            static_assert(std::is_base_of<refcount_base, Type>::value, "Type must inherit refcount_base");
            Type* _ref;
        public:
            ref() { _ref = nullptr; }
            ~ref() { free(); }

            //Construct from Pointer
            ref(Type* other) {
                if (other) other->add_ref();
                _ref = other;
            }
            //Copy from pointer
            const ref &operator = (Type *source) {
                Type *old = _ref;
                if (source) source->add_ref();
                _ref = source;
                if (old) old->release();//decrement reference and delete object if refcount == 0
                return *this;
            }

            //Construct from reference
            ref(const ref &sRef) {
                Type *source = sRef._ref;
                if (source) source->add_ref();
                _ref = source;
            }
            //Copy from reference.
            const ref &operator = (const ref &other) {
                Type *source = other._ref;
                Type *old = _ref;
                if (source) source->add_ref();
                _ref = source;
                if (old) old->release();//decrement reference and delete object if refcount == 0
                return *this;
            }

            bool isNull() const { return _ref == nullptr; }
            void free() {
                if (!_ref) return;
                _ref->release();
                _ref = nullptr;
            }
            //This returns a pointer to the underlying object. Use with caution!
            Type *getRef() const { return _ref; }
            Type *operator -> () const { return _ref; }
            operator Type *() const { return _ref; }
            bool operator != (const ref<Type>& other_) { return _ref != other_._ref; }
            size_t ref_count() const { return _ref ? _ref->ref_count() : 0; }
        };


        template<std::size_t N>
        class r_string_const : public compact_array<char> { // constexpr string
        private:
            const size_t _size;
            char _data[N];
        public:
            constexpr r_string_const(const char a[N]) : _data(a), _size(N - 1), _refcount(2) { add_ref(); }
        };

        class r_string {
            //friend constexpr const r_string& operator ""_rs(const char*, size_t);
        protected:
        public:
            r_string() {}
            r_string(std::string_view str) {
                if (str.length()) _ref = create(str.data(), str.length());
            }
            constexpr r_string(compact_array<char>* dat) : _ref(dat) {}
            template<std::size_t N>
            constexpr r_string(const r_string_const<N>& dat) : _ref(&dat) {}
            //constexpr r_string(std::string_view str) {   //https://woboq.com/blog/qstringliteral.html
            //    _ref = [str]() -> compact_array<char>* {
            //        static const r_string_const<str.length()> r_string_literal{
            //        str.data()
            //    };
            //    return reinterpret_cast<compact_array<char>>(&r_string_literal);
            //    }();
            //}
            r_string(r_string&& _move) {
                _ref = _move._ref;
                _move._ref = nullptr;
            }
            r_string(const r_string& _copy) {
                _ref = _copy._ref;
            }


            r_string& operator = (r_string&& _move) {
                if (this == &_move)
                    return *this;
                _ref = _move._ref;
                _move._ref = nullptr;
                return *this;
            }
            r_string& operator = (const r_string& _copy) {
                _ref = _copy._ref;
                return *this;
            }
            r_string& operator = (std::string_view _copy) {
                //if (!_ref || _ref.ref_count() <= 1) {
                _ref = _copy.length() ? create(_copy.data(), _copy.length()) : nullptr;
                //}
                //_ref->assign(_copy.data(), _copy.length());  //#TODO compact_array::assign. Reallocates if already has memory
                return *this;
            }
            const char* data() const {
                if (_ref) return _ref->data();
                static char empty[]{ 0 };
                return empty;
            }
            const char* c_str() const { return data(); }
            char* data_mutable() {
                if (!_ref) return nullptr;
                make_mutable();
                return _ref->data();
            }

            explicit operator const char *() const { return data(); }
            operator std::string_view() const { return std::string_view(data()); }
            //explicit operator std::string() const { return std::string(data()); } //non explicit will break string_view operator because std::string operator because it becomes ambiguous
            //This calls strlen so O(N) 
            int length() const {
                if (!_ref) return 0;
                return strlen(_ref->data());
            }

            //== is case insensitive just like scripting
            bool operator == (const char *other) const {
                return _strcmpi(data(), other) == 0;
            }

            //!= is case insensitive just like scripting
            bool operator != (const char *other) const {
                return _strcmpi(data(), other) != 0;
            }

            friend std::ostream& operator << (std::ostream& _os, const r_string& _s) {
                _os << _s.data();
                return _os;
            }

            bool compare_case_sensitive(const char *other) const {
                return _stricmp(data(), other) == 0;
            }

            size_t find(char ch, size_t start = 0) const {
                if (length() == 0) return -1;
                const char *pos = strchr(_ref->data() + start, ch);
                if (pos == nullptr) return -1;
                return pos - _ref->data();
            }

            size_t find(const char *sub, int nStart = 0) const {
                if (_ref == nullptr || length() == 0) return -1;
                const char *pos = strstr(_ref->data() + nStart, sub);
                if (pos == nullptr) return -1;
                return pos - _ref->data();
            }
            void clear() {
                _ref = nullptr;
            }
            size_t hash() const {
                return std::hash<const char*>()(data()); //#TODO string_view hash might be better...
            }

        public://#TODO make private after all rv_strings were replaced
            ref<compact_array<char>> _ref;

            static compact_array<char> *create(const char *str, int len) {
                if (len == 0 || *str == 0) return nullptr;
                compact_array<char> *string = compact_array<char>::create(len + 1);
                strncpy_s(string->data(), string->size(), str, len);//#TODO better use memcpy? does strncpy_s check for null chars? we don't want that
                string->data()[len] = 0;
                return string;
            }

            static compact_array<char> *create(int len) {
                if (len == 0) return nullptr;
                compact_array<char> *string = compact_array<char>::create(len + 1);
                string->data()[0] = 0;
                return string;
            }

            static compact_array<char> *create(const char *str) {
                if (*str == 0) return nullptr;
                return create(str, strlen(str));
            }
            void make_mutable() {
                if (_ref && _ref->ref_count() > 1) {//If there is only one reference we can safely modify the string
                    _ref = create(_ref->data());
                }
            }
        };

        //constexpr r_string tests
        //#define r_string_literal(str) \
        //    ([]() -> const r_string_const<sizeof(str)- 1>& { \
        //        static const r_string_const<sizeof(str)- 1> literal{ str }; \
        //        return literal; \
        //    }())

        //constexpr const r_string& operator ""_rs(const char* __str, size_t len) {
        //    return r_string_literal(__str);
        //};

        class rv_pool_allocator {
            char pad_0x0000[0x24]; //0x0000
        public:
            const char* _allocName;

            int _1;
            int _2;
            int _3;
            int _4;
            int allocated_count;

            void* allocate(size_t count);
            void deallocate(void* data);

        };



        class[[deprecated]] rv_string{
        public:
            rv_string() : length(0), ref_count_internal(0) {};
            rv_string(const rv_string &) = delete;
            rv_string(rv_string &&) = delete;
            rv_string & operator=(const rv_string &) = delete;
            rv_string & operator=(rv_string &&) = delete;

            uint32_t ref_count_internal;
            uint32_t length;
            char char_string; //#TODO add .natvis to display full string in Visual Studio dbg
        };



        //Contributed by ArmaDebugEngine
        class rv_arraytype {};

        template<class Type>
        class rv_array : public rv_arraytype {
        protected:
            Type *_data;
            int _n;
        public:
            rv_array() :_data(nullptr), _n(0) {};
            rv_array(rv_array<Type>&& move_) :_data(move_._data), _n(move_._n) { _move.data = nullptr; _move._n = 0; };
            Type &get(size_t i) {
                return _data[i];
            }
            const Type &get(size_t i) const {
                return _data[i];
            }
            Type &operator [] (size_t i) { return get(i); }
            const Type &operator [] (size_t i) const { return get(i); }
            Type *data() { return _data; }
            size_t count() const { return static_cast<size_t>(_n); }

            Type &front() { return get(0); }
            Type &back() { return get(_n - 1); }

            Type* begin() { return &get(0); }
            Type* end() { return &get(_n); }

            const Type* begin() const { return &get(0); }
            const Type* end() const { return &get(_n); }

            const Type &front() const { return get(0); }
            const Type &back() const { return get(_n - 1); }

            bool is_empty() const { return _n == 0; }

            template <class Func>
            void for_each(const Func &f) const {
                for (size_t i = 0; i < count(); i++) {
                    f(get(i));
                }
            }

            template <class Func>
            void for_each_backwards(const Func &f) const { //This returns if Func returns true
                for (size_t i = count() - 1; i >= 0; i--) {
                    if (f(get(i))) return;
                }
            }

            template <class Func>
            void for_each(const Func &f) {
                for (size_t i = 0; i < count(); i++) {
                    f(get(i));
                }
            }
            size_t hash() const {
                size_t _hash{ 0 };
                for (const auto& it : *this) {
                    _hash ^= std::hash<Type>()(it) + 0x9e3779b9 + (_hash << 6) + (_hash >> 2);
                }
                return _hash;
            };
        };

        template<class Type, unsigned int growthFactor = 32>
        class auto_array : public rv_array<Type> { //#TODO make writeable. This is read-only right now
            typedef rv_array<Type> base;
        protected:
            int _maxItems;
            Type *try_realloc(Type *old, int oldN, int &n) {
                Type *ret = rv_allocator<Type>::reallocate(old, n);
                return ret;
            }
            void reallocate(int size) {

                if (_maxItems == size) return;

                if (base::_n > size) {
                    resize(size);
                    return;//resize calls reallocate and reallocates... Ugly.. I know
                }
                Type *newData = nullptr;
                if (base::_data && size > 0 && (newData = try_realloc(base::_data, _maxItems, size))) {
                    _maxItems = size;
                    rv_allocator<Type>::deallocate(base::_data);
                    base::_data = newData;
                    return;
                }
                newData = rv_allocator<Type>::createUninitializedArray(size);
                if (base::_data) {
                    memmove_s(newData, size * sizeof(Type), base::_data, base::_n * sizeof(Type));
                    rv_allocator<Type>::deallocate(base::_data);
                }
                base::_data = newData;
                _maxItems = size;

            }
            void grow(size_t n) {
            #undef max
                reallocate(_maxItems + std::max(n, growthFactor));
            }
        public:

            auto_array() : rv_array<Type>(), _maxItems(0) {};
            auto_array(const std::initializer_list<Type> &init_) : rv_array<Type>(), _maxItems(0) {
                resize(init_.size());
                for (auto& it : init_)
                    push_back(it);
            }
            auto_array(auto_array<Type> &&move_) : rv_array<Type>(move_), _maxItems(move_._maxItems) {
                move_._maxItems = 0;
            }
            ~auto_array() {
                resize(0);
            }

            auto_array& operator = (auto_array &&move_) {
                base::_n = move_._n;
                _maxItems = move_._maxItems;
                base::_data = move_._data;
                move_._data = nullptr;
                move_._n = 0;
                move_._maxItems = 0;
                return *this;
            }

            auto_array& operator = (const auto_array &copy_) {
                resize(copy_._n);
                for (auto it : copy_)
                    push_back(std::move(it));
                std::vector<int> x = { 1,2,3 };
                return *this;
            }

            void resize(size_t n) {//#TODO split this between resize/reserve
                if (static_cast<int>(n) < base::_n) {
                    for (int i = static_cast<int>(n); i < base::_n; i++) {
                        (*this)[i].~Type();
                    }
                    base::_n = static_cast<int>(n);
                }
                if (n == 0 && base::_data) {
                    rv_allocator<Type>::deallocate(rv_array<Type>::_data);
                    rv_array<Type>::_data = nullptr;
                    return;
                }
                reallocate(n);
            }

            template<class... _Valty>
            Type& emplace_back(_Valty&&... _Val) {
                if (_maxItems < base::_n + 1) {
                    grow(1);
                }
                auto& item = (*this)[base::_n];
                ::new (&item) Type(std::forward<_Valty>(_Val)...);
                ++base::_n;
                return item;
            }
            Type& push_back(const Type& _Val) {
                return emplace_back(_Val);
            }
            Type& push_back(Type&& _Val) {
                return emplace_back(std::move(_Val));
            }

            void erase(int index) {
                if (index > base::_n) return;
                auto item = (*this)[index];
                item.~Type();
                memmove_s(&(*this)[index], (base::_n - index) * sizeof(Type), &(*this)[index + 1], (base::_n - index - 1) * sizeof(Type));
            }
            //#TODO Implement find, operator==, Copy/Move Contructor/Operator.

        };


        static inline unsigned int rv_map_hash_string_case_sensitive(const char *key, int hashValue = 0) {
            while (*key) {
                hashValue = hashValue * 33 + static_cast<unsigned char>(*key++);
            }
            return hashValue;
        }
        static inline unsigned int rv_map_hash_string_case_insensitive(const char *key, int hashValue = 0) {
            while (*key) {
                hashValue = hashValue * 33 + static_cast<unsigned char>(tolower(*key++));
            }
            return hashValue;
        }

        struct map_string_to_class_trait {
            static unsigned int hash_key(const char * key) {
                return rv_map_hash_string_case_sensitive(key);
            }
            static int compare_keys(const char * k1, const char * k2) {
                return strcmp(k1, k2);
            }
        };

        struct map_string_to_class_trait_caseinsensitive : public map_string_to_class_trait {
            static unsigned int hash_key(const char * key) {
                return rv_map_hash_string_case_insensitive(key);
            }

            static int compare_keys(const char * k1, const char * k2) {
                return _strcmpi(k1, k2);
            }
        };

        template <class Type, class Container, class Traits = map_string_to_class_trait>
        class map_string_to_class {
        protected:
            Container* _table;
            int _tableCount{ 0 };
            int _count{ 0 };
            static Type _null_entry;
        public:

            class iterator {
                size_t _table;
                size_t _item;
                const map_string_to_class<Type, Container, Traits> *_map;
                size_t number_of_tables() {
                    return _map->_table ? static_cast<size_t>(_map->_tableCount) : 0u;
                }
                void get_next() {
                    while (_table < number_of_tables() && _item >= _map->_table[_table].count()) {
                        _table++;
                        _item = 0;
                    }
                }
            public:
                iterator(const map_string_to_class<Type, Container, Traits> &base) {
                    _table = 0; _item = 0; _map = &base;
                    get_next();
                }
                iterator(const map_string_to_class<Type, Container, Traits> &base, bool) { //Creates end Iterator
                    _item = 0; _map = &base;
                    _table = number_of_tables();
                }
                iterator& operator++ () {
                    if (_table >= number_of_tables()) return *this;
                    ++_item;
                    get_next();
                    return *this;
                }
                iterator operator++(int) {
                    iterator _tmp = *this;
                    ++*this;
                    return (_tmp);
                }
                bool operator==(const iterator& _other) {
                    return _table == _other._table && _item == _other._item;
                }
                bool operator!=(const iterator& _other) {
                    return _table != _other._table || _item != _other._item;
                }
                const Type &operator * () const {
                    return _map->_table[_table][_item];
                }
                const Type *operator-> () const {
                    return &_map->_table[_table][_item];
                }
                Type &operator * () {
                    return _map->_table[_table][_item];
                }
                Type *operator-> () {
                    return &_map->_table[_table][_item];
                }
            };
            iterator begin() {
                return iterator(*this);
            }
            iterator end() {
                return iterator(*this, true);
            }
            const iterator begin() const {
                return iterator(*this);
            }
            const iterator end() const {
                return iterator(*this, true);
            }

            map_string_to_class() {}

            template <class Func>
            void for_each(Func func) const {
                if (!_table || !_count) return;
                for (int i = 0; i < _tableCount; i++) {
                    const Container &container = _table[i];
                    for (int j = 0; j < container.count(); j++) {
                        func(container[j]);
                    }
                }
            }

            template <class Func>
            void for_each_backwards(Func func) const {
                if (!_table || !_count) return;
                for (int i = _tableCount - 1; i >= 0; i--) {
                    const Container &container = _table[i];
                    for (int j = container.count() - 1; j >= 0; j--) {
                        func(container[j]);
                    }
                }
            }

            const Type &get(const char* key) const {
                if (!_table || !_count) return _null_entry;
                int hashedKey = hash_key(key);
                for (int i = 0; i < _table[hashedKey].count(); i++) {
                    const Type &item = _table[hashedKey][i];
                    if (Traits::compare_keys(item.get_map_key(), key) == 0)
                        return item;
                }
                return _null_entry;
            }

            Container* get_table_for_key(const char* key) {
                if (!_table || !_count) return nullptr;
                int hashedKey = hash_key(key);
                return &_table[hashedKey];
            }

            Type &get(const char* key) {
                if (!_table || !_count) return _null_entry;
                int hashedKey = hash_key(key);
                for (size_t i = 0; i < _table[hashedKey].count(); i++) {
                    Type &item = _table[hashedKey][i];
                    if (Traits::compare_keys(item.get_map_key(), key) == 0)
                        return item;
                }
                return _null_entry;
            }

            static bool is_null(const Type &value) { return &value == &_null_entry; }

            bool has_key(const char* key) const {
                return !is_null(get(key));
            }

        public:
            int count() const { return _count; }
        protected:
            int hash_key(const char* key) const {
                return Traits::hash_key(key) % _tableCount;
            }
        };

        template<class Type, class Container, class Traits>
        Type map_string_to_class<Type, Container, Traits>::_null_entry;







        struct value_entry {
            r_string type_name;
        };

        struct compound_value_pair {
            value_entry *first;
            value_entry *second;
        };

        struct compound_value_entry {
            uintptr_t           v_table;
            compound_value_pair *types;
        };

        class op_value_entry {
        public:
            uintptr_t               v_table;
            value_entry             *single_type;
            compound_value_entry    *compound_type;
            value_types type() const;
            std::string type_str() const;
            bool operator==(const op_value_entry& other) const {
                return single_type == other.single_type && compound_type == other.compound_type;
            }
            bool operator!=(const op_value_entry& other) const {
                return single_type != other.single_type || compound_type != other.compound_type;
            }
        };

        struct unary_operator {
            uintptr_t        v_table;
            uint32_t         ref_count;
            unary_function   *procedure_addr;
            op_value_entry   return_type;
            op_value_entry   arg_type;
        };

        struct unary_entry {
            const char *name;
            uintptr_t procedure_ptr_addr;
            unary_operator *op;
        };

        struct binary_operator {
            uintptr_t        v_table;
            uint32_t         ref_count;
            binary_function   *procedure_addr;
            op_value_entry   return_type;
            op_value_entry   arg1_type;
            op_value_entry   arg2_type;
        };

        struct binary_entry {
            const char *name;
            uintptr_t procedure_ptr_addr;
            binary_operator *op;
        };

        struct nular_operator {
            uintptr_t        v_table;
            uint32_t         ref_count;
            nular_function   *procedure_addr;
            op_value_entry   return_type;
        };

        struct nular_entry {
            const char *name;
            uintptr_t procedure_ptr_addr;
            nular_operator *op;
        };

        class I_debug_value {  //ArmaDebugEngine is very helpful... (No advertising.. I swear!)
        public:                //#TODO move this into __internal
            I_debug_value() {}
            virtual ~I_debug_value() {}
            // IDebugValue
            virtual int IAddRef() = 0;
            virtual int IRelease() = 0;
        };
        class game_value;
        class game_data : public refcount, public I_debug_value {
            friend class game_value;
        public:
            virtual const op_value_entry & type() const { static op_value_entry dummy; return dummy; }//#TODO replace op_value_entry by some better name
            virtual ~game_data() {}

        protected:
            virtual bool get_as_bool() const { return false; }
            virtual float get_as_number() const { return 0.f; }
            virtual r_string get_as_string() const { return ""; } //Only usable on String and Code! Use to_string instead!
            virtual const auto_array<game_value>& get_as_const_array() const { static auto_array<game_value> dummy; return dummy; } //Why would you ever need this?
            virtual auto_array<game_value> &get_as_array() { static auto_array<game_value> dummy; return dummy; }
            virtual game_data *copy() const { return NULL; }
            virtual void set_readonly(bool val) {} //No clue what this does...
            virtual bool get_readonly() const { return false; }
            virtual bool get_final() const { return false; }
            virtual void set_final(bool val) {}; //Only on GameDataCode AFAIK
            virtual r_string to_string() const { return ""; }
            virtual bool equals(const game_data *data) const { return false; };
            virtual const char *type_as_string() const { return "unknown"; }
            virtual bool is_nil() const { return false; }

            int IAddRef() override { return add_ref(); };
            int IRelease() override { return release(); };
            uintptr_t get_vtable() const {
                return *reinterpret_cast<const uintptr_t*>(this);
            }
        };

        class game_data_number : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            static rv_pool_allocator* pool_alloc_base;
            game_data_number();
            game_data_number(float val_);
            game_data_number(const game_data_number &copy_);
            game_data_number(game_data_number &&move_);
            game_data_number & game_data_number::operator = (const game_data_number &copy_);
            game_data_number & game_data_number::operator = (game_data_number &&move_);
            static void* operator new(std::size_t sz_);
            static void operator delete(void* ptr_, std::size_t sz_);
            float number;
            size_t hash() const {
                return __internal::pairhash(type_def, number);
            };
        protected:
            static thread_local game_data_pool<game_data_number> _data_pool;
        };

        class game_data_bool : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            static rv_pool_allocator* pool_alloc_base;
            game_data_bool();
            game_data_bool(bool val_);
            game_data_bool(const game_data_bool &copy_);
            game_data_bool(game_data_bool &&move_);
            game_data_bool & game_data_bool::operator = (const game_data_bool &copy_);
            game_data_bool & game_data_bool::operator = (game_data_bool &&move_);
            static void* operator new(std::size_t sz_);
            static void operator delete(void* ptr_, std::size_t sz_);
            bool val;
            size_t hash() const { return __internal::pairhash(type_def, val); };
        protected:
            static thread_local game_data_pool<game_data_bool> _data_pool;
        };

        class[[deprecated]] rv_game_value{
        };

        class game_data_array;
        class internal_object;

        class __game_value_vtable_dummy {
        public:
            virtual void __dummy_vtable(void) {};
        };

        class game_value : public __game_value_vtable_dummy {
        public:
            static uintptr_t __vptr_def;//#TODO make private and add friend classes
            game_value();
            ~game_value();
            void copy(const game_value & copy_); //I don't see any use for this.
            game_value(const game_value &copy_);
            game_value(game_value &&move_);



            //Conversions
            game_value(float val_);
            game_value(bool val_);
            game_value(const std::string &val_);
            game_value(const char *);
            game_value(const std::vector<game_value> &list_);
            game_value(const std::initializer_list<game_value> &list_);
            game_value(auto_array<game_value> &&array_);
            game_value(const vector3 &vec_);
            game_value(const vector2 &vec_);
            game_value(const internal_object &internal_);

            game_value & operator = (const game_value &copy_);
            game_value & operator = (game_value &&move_);
            game_value & operator = (float val_);
            game_value & operator = (bool val_);
            game_value & operator = (const std::string &val_);
            game_value & operator = (const r_string &val_);
            game_value & operator = (const char *val_);
            game_value & operator = (const std::vector<game_value> &list_);
            game_value & operator = (const vector3 &vec_);
            game_value & operator = (const vector2 &vec_);
            game_value & operator = (const internal_object &internal_);


            operator int() const;
            operator float() const;
            operator bool() const;
            operator std::string() const;
            operator r_string() const;
            operator vector3() const;
            operator vector2() const;
            //#TODO add operator to const auto_array ref and then replace stuff with foreach loops

            game_value& operator [](size_t i_);
            game_value operator [](size_t i_) const;

            uintptr_t type() const;//#TODO return GameDataType

            [[deprecated("Replaced by size, because that name is more clear")]] size_t length() const;
            size_t size() const;
            //#TODO implement is_null. GameDataObject's objectLink == nullptr. Same for GameDataGroup and others.
            //Returns true for uninitialized game_value's and Nil values returned from Arma
            bool is_nil() const;

            bool operator==(const game_value& other) const;
            bool operator!=(const game_value& other) const;


            size_t hash() const;

            ref<game_data> data;
            [[deprecated]] static void* operator new(std::size_t sz_); //Should never be used
            static void operator delete(void* ptr_, std::size_t sz_);
        protected:
            uintptr_t get_vtable() const;
            void set_vtable(uintptr_t vt);

        };

        class game_data_array : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            static rv_pool_allocator* pool_alloc_base;
            game_data_array();
            game_data_array(size_t size_);
            game_data_array(const std::vector<game_value> &init_);
            game_data_array(const std::initializer_list<game_value> &init_);
            game_data_array(auto_array<game_value> &&init_);
            game_data_array(const game_data_array &copy_);
            game_data_array(game_data_array &&move_);
            game_data_array & game_data_array::operator = (const game_data_array &copy_);
            game_data_array & game_data_array::operator = (game_data_array &&move_);
            ~game_data_array();
            auto_array<game_value> data;
            auto length() { return data.count(); }
            size_t hash() const { return __internal::pairhash(type_def, data.hash()); };
            static void* operator new(std::size_t sz_);
            static void operator delete(void* ptr_, std::size_t sz_);
        };

        class game_data_string : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            static rv_pool_allocator* pool_alloc_base;
            game_data_string();
            game_data_string(const std::string &str_);
            game_data_string(const r_string &str_);
            game_data_string(const game_data_string &copy_);
            game_data_string(game_data_string &&move_);
            game_data_string & game_data_string::operator = (const game_data_string &copy_);
            game_data_string & game_data_string::operator = (game_data_string &&move_);
            ~game_data_string();
            r_string raw_string;
            size_t hash() const { return __internal::pairhash(type_def, raw_string); };
            static void* operator new(std::size_t sz_);
            static void operator delete(void* ptr_, std::size_t sz_);
        protected:
            static thread_local game_data_pool<game_data_string> _data_pool;
        };

        class game_data_group : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_group() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, group); };
            void *group;
        };

        class game_data_config : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_config() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, config); };
            void *config;
        };

        class game_data_control : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_control() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, control); };
            void *control;
        };

        class game_data_display : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_display() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, display); };
            void *display;
        };

        class game_data_location : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_location() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, location); };
            void *location;
        };

        class game_data_script : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_script() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, script); };//#TODO might want to hash script string
            void *script;
        };

        class game_data_side : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_side() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, side ); };
            void *side;
        };

        class game_data_rv_text : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_rv_text() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, rv_text ); };
            void *rv_text;
        };

        class game_data_team : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_team() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, team ); };
            void *team;
        };

        class game_data_rv_namespace : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_rv_namespace() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, rv_namespace ); };
            void *rv_namespace;
        };

        class game_data_code : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_code() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, code_string ); };
            r_string code_string;
            uintptr_t instruction_array;
            uint32_t instruction_array_size;
            uint32_t instruction_array_max_size;
            bool is_final;
        };

        class game_data_object : public game_data {
        public:
            static uintptr_t type_def;
            static uintptr_t data_type_def;
            game_data_object() {
                *reinterpret_cast<uintptr_t*>(this) = type_def;
                *reinterpret_cast<uintptr_t*>(static_cast<I_debug_value*>(this)) = data_type_def;
            };
            size_t hash() const { return __internal::pairhash(type_def, object ); };
            void *object;
        };

        template<size_t Size = 1024, size_t Alloc_Length = 512>
        class game_data_string_pool {
        public:
            game_data_string_pool() : _running(false) {
                for (size_t i = 0; i < Size; ++i)
                    _buy_entry(Alloc_Length);
            }

            inline r_string acquire(size_t alloc_length_) {
                std::lock_guard<std::mutex> collector_lock(_string_collection_mutex);
                if (_pool_queue.size() == 0 || alloc_length_ > Alloc_Length)
                    _buy_entry(alloc_length_);
                r_string ret = _pool_queue.front();
                _pool_queue.pop();
                return ret;
            }

            inline void release(r_string _ptr) {
                /*
                std::lock_guard<std::mutex> collector_lock(_string_collection_mutex);
                if (_ptr->ref_count_internal <= 1) {
                    _ptr->ref_count_internal = 0;
                    _ptr->length = 0;
                    _ptr->char_string = 0x0;
                    _pool_queue.push(_ptr);
                } else {
                    if (!_running) {
                        _running = true;
                        _collection_thread = std::thread(&game_data_string_pool::_string_collector, this);
                    }
                    _string_collection.push_back(_ptr);
                }
                */
            }

            ~game_data_string_pool() {
                _running = false;
                _collection_thread.join();
            }
        protected:
            std::queue<r_string> _pool_queue;
            std::vector<r_string> _pool;
            std::mutex _string_collection_mutex;
            std::thread _collection_thread;
            std::atomic<bool> _running;

            inline void _buy_entry(size_t alloc_length_) {
                r_string str;
                _pool_queue.push(str);
                _pool.push_back(str);
            }

            /*!
            @brief A list of strings to monitor for deletion, used by the string collector.
            */
            std::list<r_string> _string_collection;

            /*!
            @brief Collects a string and adds it to a queue that waits for its internal
            ref counter to reach 1, at which point it can be safely released.

            Strings allocated by Intercept are allocated in the Intercept host memory
            because the engine may or may not use them right away. In some cases the
            string may be held in some sort of execution logic for a significant time.
            Since these strings are normal internal strings, the engine will adjust
            their ref counter to account for this, and we need to monitor the ref counter
            to make sure that we can safely release it.

            @param str_ A pointer to the string to release.
            */
            void _string_collector() {
                /*
                while (_running) {
                    {
                        std::lock_guard<std::mutex> collector_lock(_string_collection_mutex);
                        for (auto it = _string_collection.begin(); it != _string_collection.end();) {
                            if ((*it)->ref_count_internal <= 1) {
                                rv_string *ptr = *it;
                                ptr->ref_count_internal = 0;
                                ptr->length = 0;
                                ptr->char_string = 0x0;
                                _pool_queue.push(ptr);
                                _string_collection.erase(it++);
                            } else {
                                ++it;
                            }
                        }
                    }
                    sleep(250);
                }
                */
            }
        };


        namespace __internal {
            enum class GameDataType {
                SCALAR,
                BOOL,
                ARRAY,
                STRING,
                NOTHING,
                ANY,
                NAMESPACE,
                NaN,
                CODE,
                OBJECT,
                SIDE,
                GROUP,
                TEXT,
                SCRIPT,
                TARGET,
                CONFIG,
                DISPLAY,
                CONTROL,
                NetObject,
                SUBGROUP,
                TEAM_MEMBER,
                TASK,
                DIARY_RECORD,
                LOCATION,
                end
            };
            GameDataType game_datatype_from_string(const r_string& name);
            std::string to_string(GameDataType type);

            struct allocatorInfo {
                uintptr_t genericAllocBase;
                uintptr_t poolFuncAlloc;
                uintptr_t poolFuncDealloc;
                std::array<rv_pool_allocator*, static_cast<size_t>(GameDataType::end)> _poolAllocs;
            };
        }

        class registered_sqf_function {
            friend class sqf_functions;
        public:
            registered_sqf_function() {};
            registered_sqf_function(std::shared_ptr<registered_sqf_function_impl> func_);
        private:
            std::shared_ptr<registered_sqf_function_impl> _function;
        };

        template <game_value(*T)(game_value, game_value)>
        static uintptr_t userFunctionWrapper(char* sqf_this_, uintptr_t, uintptr_t left_arg_, uintptr_t right_arg_) {
            game_value* l = reinterpret_cast<game_value*>(left_arg_);
            game_value* r = reinterpret_cast<game_value*>(right_arg_);
            ::new (sqf_this_) game_value(T(*l, *r));
            return reinterpret_cast<uintptr_t>(sqf_this_);
        }

        template <game_value(*T)(game_value)>
        static uintptr_t userFunctionWrapper(char* sqf_this_, uintptr_t, uintptr_t right_arg_) {
            game_value* r = reinterpret_cast<game_value*>(right_arg_);
            ::new (sqf_this_) game_value(T(*r));
            return reinterpret_cast<uintptr_t>(sqf_this_);
        }
    }
}

namespace std {
    template <> struct hash<intercept::types::r_string> {
        size_t operator()(const intercept::types::r_string& x) const {
            return x.hash();
        }
    };
    template <> struct hash<intercept::types::game_value> {
        size_t operator()(const intercept::types::game_value& x) const {
            return x.hash();
        }
    };
}


