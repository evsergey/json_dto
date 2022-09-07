#pragma once

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/writer.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace json_dto
{
class parse_exception : public std::exception
{
    std::string _reason;
public:
    explicit parse_exception(std::string reason) : _reason(reason) {}
    explicit parse_exception(const rapidjson::ParseResult& pr)
    {
        std::stringstream ss;
        rapidjson::GetParseErrorFunc GetParseError = rapidjson::GetParseError_En;
        ss << "Parse error: " << pr.Code() << "(" << GetParseError(pr.Code()) << "), at " << pr.Offset();
        _reason = ss.str();
    }
    [[nodiscard]] char const* what() const noexcept override { return _reason.c_str(); }
};

using value_r = rapidjson::Value&;
using value_c = const rapidjson::Value&;
using allocator = rapidjson::MemoryPoolAllocator<>;
template<class T, class IO>
concept io_supported = requires (T value, IO& io)
{
    value.serialization(io);
};

class json_reader
{
    const rapidjson::Value& _v;
    const char* _type_name = nullptr;
public:
    explicit json_reader(const rapidjson::Value& value) : _v{ value } {}
    json_reader& operator()(const char* name) { _type_name = name; return *this; }
    template<class T>
    const json_reader& operator()(const char* name, T& value) const;
    template<class T>
    const json_reader& operator()(const char* name, std::decay_t<T>* p_value) const;
    template<class T, std::convertible_to<T> TT>
        requires(!std::assignable_from<T, TT>)
    const json_reader& operator()(const char* name, T& value, const TT& default_value) const;
    template<class TT, std::assignable_from<TT> T>
    const json_reader& operator()(const char* name, T& value, const TT& default_value) const;
    template<class T, class TT>
        requires std::is_convertible_v<std::invoke_result_t<TT>, T>
    const json_reader& operator()(const char* name, std::decay_t<T>& value, TT default_value_maker) const;
};

template<class T, class U>
concept has_equal_with = requires(const std::remove_reference_t<T>&x, const std::remove_reference_t<U>&y)
{
    { x == y } -> std::convertible_to<bool>;
};

class json_writer
{
    rapidjson::Value& _v;
    allocator& _a;
public:
    json_writer(rapidjson::Value& value, allocator& allocator) : _v{ value }, _a{ allocator } {}
    json_writer& operator()([[maybe_unused]] const char* name) { return *this; }
    template<class T>
    json_writer& operator()(const char* name, const T& value);
    template<class T>
    json_writer& operator()(const char* name, const std::decay_t<T>* p_value);
    template<class T, std::convertible_to<T> TT>
        requires(!has_equal_with<T, TT>)
    json_writer& operator()(const char* name, const T& value, TT default_value);
    template<class T, has_equal_with<T> TT>
    json_writer& operator()(const char* name, const T& value, TT default_value);
    template<class T, class TT>
        requires std::is_convertible_v<std::invoke_result_t<TT>, T>
    json_writer& operator()(const char* name, const T& value, TT default_value_maker);
};

template<class T>
struct adapter
{
    using struct_like = void;
    static bool get(value_c v, T& value)
    {
        if (!v.IsObject())
            return false;
        json_reader reader{ v };
        value.serialization(reader);
        return true;
    }
    static void set(allocator& a, value_r v, const T& value)
    {
        if(!v.IsObject())
            v.SetObject();
        json_writer writer { v, a };
        auto& mvalue = const_cast<T&>(value);
        mvalue.serialization(writer);
    }
};

template<class T>
concept struct_like = std::is_same_v<void, typename adapter<T>::struct_like>;

#define ADAPTER(CPP_TYPE, TYPE) \
template<> \
struct adapter<CPP_TYPE> { \
static bool get(value_c v, CPP_TYPE& value) { \
    if(!v.Is##TYPE ()) return false; \
    value = (CPP_TYPE)v.Get##TYPE (); \
    return true; \
} \
static void set([[maybe_unused]] allocator& a, value_r v, CPP_TYPE value) { v.Set##TYPE (value); } };

ADAPTER(int, Int)
ADAPTER(unsigned int, Uint)
ADAPTER(int64_t, Int64)
ADAPTER(uint64_t, Uint64)
ADAPTER(float, Float)
ADAPTER(double, Double)
ADAPTER(bool, Bool)
#undef ADAPTER

template<>
struct adapter<std::string>
{
    static bool get(value_c v, std::string& value)
    {
        if (!v.IsString())
            return false;
        value = { v.GetString(), v.GetStringLength() };
        return true;
    }
    static void set(allocator& a, value_r v, const std::string& value)
    {
        v.SetString(value.data(), (rapidjson::SizeType)value.size(), a);
    }
};

template<class T> concept sizable = requires(const T & x) { {x.size()}->std::same_as<size_t>; };

template<class T>
concept bitset_like = sizable<T> && requires(T v, const T cv)
{
    { T{ "", 0 } } -> std::same_as<T>;
    { T{ 0ull } } -> std::same_as<T>;
    v[0] = true;
    { cv[0] } -> std::same_as<bool>;
    { cv.to_ullong() } -> std::same_as<unsigned long long>;
    { cv.to_string() } -> std::same_as<std::string>;
};

template<bitset_like BS>
struct adapter<BS>
{
    static bool get(value_c v, BS& value)
    {
        if (v.IsString())
        {
            value = BS{ v.GetString(), v.GetStringLength() };
            return true;
        }
        if (value.size() <= 64)
        {
            if (v.IsUint64())
            {
                value = BS{ v.GetUint64() };
                return true;
            }
        }
        return false;
    }
    static void set(allocator& a, value_r v, const BS& value)
    {
        if (value.size() <= 64)
            v.SetUint64(value.to_ullong());
        else
        {
            const auto str = value.to_string();
            v.SetString(str.data(), (rapidjson::SizeType)str.size(), a);
        }
    }
};

template<class T>
struct variant_indexer
{
    using type = size_t;
};

template<class... T>
struct adapter<std::variant<T...>>
{
    using var = std::variant<T...>;
    using indexer = typename variant_indexer<var>::type;

    static bool get(value_c v, var& value)
    {
        if (!v.IsObject())
            return false;
        auto obj = v.GetObject();
        auto typeMember = obj.FindMember("type");
        if (typeMember == obj.MemberEnd())
            return false;
        indexer type;
        if (!adapter<indexer>::get(typeMember->value, type))
            return false;
        const auto index = (size_t)type;
        return load(obj, value, index, std::make_integer_sequence<size_t, std::variant_size_v<var>>{});
    }

    template<size_t N>
    static bool load_one(const rapidjson::Value::ConstObject& obj, var& value)
    {
        using alt_t = std::variant_alternative_t<N, var>;
        alt_t alt;
        if constexpr (!struct_like<alt_t>)
        {
            auto dataMember = obj.FindMember("value");
            if (dataMember == obj.MemberEnd() || !adapter<alt_t>::get(dataMember->value, alt))
                return false;
        }
        else
        {
            if (!adapter<alt_t>::get(obj, alt))
                return false;
        }
        value = std::move(alt);
        return true;
    }

    template<size_t... N>
    static bool load(const rapidjson::Value::ConstObject& obj, var& value, size_t index, std::integer_sequence<size_t, N...>)
    {
        return ((index == N && load_one<N>(obj, value)) || ...);
    }
    static void set(allocator& a, value_r v, const var& value)
    {
        std::visit([&]<class alt_t>(const alt_t& val) {
            rapidjson::Value typeMember;
            const auto type = (indexer)value.index();
            adapter<indexer>::set(a, typeMember, type);
            if constexpr (struct_like<alt_t>)
            {
                v.SetObject();
                v.AddMember("type", typeMember, a);
                adapter<alt_t>::set(a, v, val);
            }
            else
            {
                auto& obj = v.SetObject();
                rapidjson::Value dataMember;
                obj.AddMember("type", typeMember, a);
                adapter<alt_t>::set(a, dataMember, val);
                obj.AddMember("value", dataMember, a);
            }
            }, value);
    }
};

template<class Enum>
    requires std::is_enum_v<Enum>
struct enum_names { static constexpr bool empty = true; };

template<class Enum>
concept enum_with_max = std::is_enum_v<Enum> && requires(Enum e)
{
    { Enum::max } -> std::same_as<Enum>;
};

template<class Enum>
    requires std::is_enum_v<Enum> && enum_names<Enum>::empty
struct adapter<Enum>
{
    using u_type = std::underlying_type_t<Enum>;
    static bool get(value_c v, Enum& value)
    {
        u_type underlying;
        if (!adapter<u_type>::get(v, underlying))
            return false;
        if constexpr (enum_with_max<Enum>)
        {
            if (underlying < 0 || underlying >= static_cast<u_type>(Enum::max))
                return false;
        }
        value = static_cast<Enum>(underlying);
        return true;
    }
    static void set(allocator& a, value_r v, Enum value)
    {
        adapter<u_type>::set(a, v, static_cast<u_type>(value));
    }
};
template<class Enum>
    requires std::is_enum_v<Enum> && std::is_same_v<const char*, typename decltype(enum_names<Enum>::get_names())::value_type>
struct adapter<Enum>
{
    static std::optional<Enum> convert(const std::string& name)
    {
        static const auto name_to_val = []()
        {
            std::unordered_map<std::string, Enum> res;
            for (size_t i = 0; auto name: enum_names<Enum>::get_names())
                res.emplace(name, (Enum)i++);
            return res;
        }();
        
        if (auto it = name_to_val.find(name); it != name_to_val.end())
            return it->second;
        return std::nullopt;
    }
    static bool get(value_c v, Enum& value)
    {
        std::string name;
        if (!adapter<std::string>::get(v, name))
            return false;
        if(auto val = convert(name); val)
        {
            value = val.value();
            return true;
        }
        return false;
    }
    static void set(allocator& a, value_r v, Enum value)
    {
        adapter<std::string>::set(a, v, enum_names<Enum>::get_names()[(size_t)value]);
    }
};

template<class T, size_t N>
class array_with_size
{
public:
    using value_type = T;
    using array_type = std::array<T, N>;

    array_with_size(array_type& array, size_t& sz) :
        _array(array),
        _size(sz)
    {}

    T& operator[](size_t i) { return _array[i]; }
    const T& operator[](size_t i) const { return _array[i]; }
    void clear() { _size = 0; }
    void resize(size_t sz)
    {
        if(sz > N)
            throw parse_exception("Too large array");
        _size = sz;
    }
    auto begin() { return _array.begin(); }
    auto begin() const { return _array.begin(); }
    auto cbegin() const { return _array.cbegin(); }
    auto end() { return _array.begin() + _size; }
    auto end() const { return _array.begin() + _size; }
    auto cend() const { return _array.cbegin() + _size; }
    size_t size() const
    {
        assert(_size >= 0 && _size <= N);
        return std::clamp(_size, (size_t)0, N);
    }
    template<class TT>
    bool operator==(const TT& a) const
    {
        return std::equal(_array.cbegin(), _array.cbegin() + _size, a.begin(), a.end());
    }
    template<class TT, size_t M>
    array_with_size& operator=(const std::array<TT, M>& a) const
    {
        static_assert(M <= N, "Too large array");
        _size = M;
        std::copy(a.cbegin(), a.cend(), _array.begin());
        return *this;
    }
private:
    array_type& _array;
    size_t& _size;
};

template<class T>
concept array_like = sizable<T> && requires(T v, const T cv)
{
    { v[0] } -> std::same_as<typename T::value_type&>;
    { cv[0] } -> std::same_as<const typename T::value_type&>;
};

template<class T>
concept resizable = requires(T v)
{
    v.clear();
    v.resize((size_t)0);
};

template<class T>
concept fillable = requires(T v)
{
    v.fill(typename T::value_type{});
};

template<class T>
concept reservable = requires(T v)
{
    v.reserve((size_t)0);
};

template<array_like A>
struct adapter<A>
{
    static bool get(value_c v, A& value)
    {
        if (!v.IsArray())
            return false;
        auto arr = v.GetArray();
        if constexpr (resizable<A>)
        {
            value.clear();
            value.resize(arr.Size());
        }
        else
        {
            if (value.size() < (size_t)arr.Size())
                return false;
            if constexpr (fillable<A>)
                value.fill({});
        }
        for (size_t i = 0; i < (size_t)arr.Size(); ++i)
            if (!adapter<typename A::value_type>::get(arr[(rapidjson::SizeType)i], value[i]))
                return false;
        return true;
    }
    static void set(allocator& a, value_r v, const A& value)
    {
        auto& items = v.SetArray();
        items.Reserve((rapidjson::SizeType)value.size(), a);
        for (size_t i = 0; i < value.size(); ++i)
        {
            rapidjson::Value item;
            adapter<typename A::value_type>::set(a, item, value[i]);
            items.PushBack(item, a);
        }
    }
};

template<class T>
concept map_like = sizable<T> && std::forward_iterator<typename T::const_iterator> &&
    requires(T & m, const T & cm, std::pair<typename T::key_type, typename T::mapped_type>& p)
{
    { m[typename T::key_type{}] } -> std::same_as<typename T::mapped_type&>;
    { cm[typename T::key_type{}] } -> std::same_as<const typename T::mapped_type&>;
    m.emplace(typename T::key_type{}, typename T::mapped_type{});
    { cm.begin() } -> std::same_as<typename T::const_iterator>;
    { cm.end() } -> std::same_as<typename T::const_iterator>;
    p = *cm.begin();
    p = *cm.end();
};

template<map_like M>
struct adapter<M>
{
    static bool get(value_c v, M& value)
    {
        if (!v.IsObject())
            return false;
        auto m = v.GetObject();
        value.clear();
        if constexpr (reservable<M>)
            value.reserve(m.MemberCount());
        for (auto& vi : m)
        {
            typename M::mapped_type item;
            if (!adapter<typename M::mapped_type>::get(vi.value, item))
                return false;
            typename M::key_type key;
            if (!adapter<typename M::key_type>::get(vi.name, key))
                return false;
            value.emplace(std::move(key), std::move(item));
        }
        return true;
    }
    static void set(allocator& a, value_r v, const M& value)
    {
        auto& items = v.SetObject();
        items.MemberReserve(value.size(), a);
        for (auto& [k, val] : value)
        {
            rapidjson::Value item, key;
            adapter<typename M::mapped_type>::set(a, item, val);
            adapter<typename M::key_type>::set(a, key, k);
            items.AddMember(key, item, a);
        }
    }
};

template<class T>
struct adapter<std::shared_ptr<T>>
{
    static bool get(value_c v, std::shared_ptr<T>& value)
    {
        if (v.IsNull())
        {
            value.reset();
            return true;
        }
        T x;
        if (!adapter<T>::get(v, x))
            return false;
        value = std::make_shared<T>(std::move(x));
        return true;
    }
    static void set(allocator& a, value_r v, const std::shared_ptr<T>& value)
    {
        if (!value)
            v.SetNull();
        else
            adapter<T>::set(a, v, *value);
    }
};

template<class T>
struct adapter<std::unique_ptr<T>>
{
    static bool get(value_c v, std::unique_ptr<T>& value)
    {
        if (v.IsNull())
        {
            value.reset();
            return true;
        }
        T x;
        if (!adapter<T>::get(v, x))
            return false;
        value = std::make_unique<T>(std::move(x));
        return true;
    }
    static void set(allocator& a, value_r v, const std::unique_ptr<T>& value)
    {
        if (!value)
            v.SetNull();
        else
            adapter<T>::set(a, v, *value);
    }
};

template<class T>
struct adapter<T*>
{
    static bool get(value_c v, T* value)
    {
        if (!value)
            return true;
        if (v.IsNull())
        {
            *value = T{};
            return true;
        }
        return adapter<std::remove_cv_t<T>>::get(v, *value);
    }
    static void set(allocator& a, value_r v, const T* value)
    {
        if (!value)
            v.SetNull();
        else
            adapter<std::remove_cv_t<T>>::set(a, v, *value);
    }
};

template<class T>
struct adapter<std::optional<T>>
{
    static bool get(value_c v, std::optional<T>& value)
    {
        if (v.IsNull())
        {
            value = std::nullopt;
            return true;
        }
        T x;
        if (!adapter<T>::get(v, x))
            return false;
        value.reset(std::move(x));
        return true;
    }
    static void set(allocator& a, value_r v, const std::optional<T>& value)
    {
        if (!value)
            v.SetNull();
        else
            adapter<T>::set(a, v, *value);
    }
};

template<class T>
concept with_backend = requires(const T & cx)
{
    cx.get_backend();
};

template<with_backend WB>
struct adapter<WB>
{
    using backend_type = std::decay_t<decltype(std::declval<WB>().get_backend())>;
    static bool get(value_c v, WB& value)
    {
        return adapter<backend_type>::get(v, value.get_backend());
    }
    static void set(allocator& a, value_r v, const WB& value)
    {
        adapter<backend_type>::set(a, v, value.get_backend());
    }
};

template<class T>
const json_reader& json_reader::operator()(const char* name, T& value) const
{
    auto member = _v.FindMember(name);
    if (member == _v.MemberEnd())
        throw parse_exception(std::string("Field not found: ") + name + " in type " + _type_name );
    if (!adapter<T>::get(member->value, value))
        throw parse_exception(std::string("Cannot parse field: ") + name + " in type " + _type_name);
    return *this;
}
template<class T>
const json_reader& json_reader::operator()(const char* name, std::decay_t<T>* p_value) const
{
    if (p_value == nullptr)
        return *this;
    auto member = _v.FindMember(name);
    if (member == _v.MemberEnd())
        throw parse_exception(std::string("Field not found: ") + name + " in type " + _type_name);
    if (!adapter<T>::get(member->value, *p_value))
        throw parse_exception(std::string("Cannot parse field: ") + name + " in type " + _type_name);
    return *this;
}
template<class T, std::convertible_to<T> TT>
    requires(!std::assignable_from<T, TT>)
const json_reader& json_reader::operator()(const char* name, T& value, const TT& default_value) const
{
    if (auto member = _v.FindMember(name); member == _v.MemberEnd())
        value = static_cast<T>(default_value);
    else if (!adapter<T>::get(member->value, value))
        throw parse_exception(std::string("Cannot parse field: ") + name + " in type " + _type_name);
    return *this;
}
template<class TT, std::assignable_from<TT> T>
const json_reader& json_reader::operator()(const char* name, T& value, const TT& default_value) const
{
    if (auto member = _v.FindMember(name); member == _v.MemberEnd())
        value = default_value;
    else if (!adapter<T>::get(member->value, value))
        throw parse_exception(std::string("Cannot parse field: ") + name + " in type " + _type_name);
    return *this;
}
template<class T, class TT>
    requires std::is_convertible_v<std::invoke_result_t<TT>, T>
const json_reader& json_reader::operator()(const char* name, std::decay_t<T>& value, TT default_value_maker) const
{
    if (auto member = _v.FindMember(name); member == _v.MemberEnd())
        value = static_cast<T>(default_value_maker());
    else if (!adapter<T>::get(member->value, value))
        throw parse_exception(std::string("Cannot parse field: ") + name + " in type " + _type_name);
    return *this;
}

template<class T>
T loads(std::string_view str)
{
    rapidjson::Document doc;
    if (rapidjson::ParseResult pr = doc.Parse(str.data(), str.size()); pr.IsError())
        throw parse_exception(pr);
    T result;
    if (!adapter<T>::get(doc, result))
        throw parse_exception("Cannot convert the value");
    return result;
}

template<class T>
void load(std::istream& str, T& result)
{
    rapidjson::Document doc;
    rapidjson::IStreamWrapper strw(str);
    if (rapidjson::ParseResult pr = doc.ParseStream(strw); pr.IsError())
        throw parse_exception(pr);
    if(!adapter<T>::get(doc, result))
        throw parse_exception("Cannot convert the value");
}
template<class T>
T load(std::istream& str)
{
    T result;
    load(str, result);
    return result;
}

template<class T>
json_writer& json_writer::operator()(const char* name, const T& value)
{
    rapidjson::Value key, v;
    key.SetString(name, _a);
    adapter<T>::set(_a, v, value);
    _v.AddMember(key, v, _a);
    return *this;
}
template<class T>
json_writer& json_writer::operator()(const char* name, const std::decay_t<T>* p_value)
{
    if (p_value == nullptr)
        return *this;
    rapidjson::Value key, v;
    key.SetString(name, _a);
    adapter<T>::set(_a, v, *p_value);
    _v.AddMember(key, v, _a);
    return *this;
}
template<class T, std::convertible_to<T> TT>
    requires(!has_equal_with<T, TT>)
json_writer& json_writer::operator()(const char* name, const T& value, TT default_value)
{
    if (value == (T)default_value)
        return *this;
    rapidjson::Value key, v;
    key.SetString(name, _a);
    adapter<std::remove_cv_t<T>>::set(_a, v, value);
    _v.AddMember(key, v, _a);
    return *this;
}
template<class T, has_equal_with<T> TT>
json_writer& json_writer::operator()(const char* name, const T& value, TT default_value)
{
    if (value == default_value)
        return *this;
    rapidjson::Value key, v;
    key.SetString(name, _a);
    adapter<std::remove_cv_t<T>>::set(_a, v, value);
    _v.AddMember(key, v, _a);
    return *this;
}
template<class T, class TT>
    requires std::is_convertible_v<std::invoke_result_t<TT>, T>
json_writer& json_writer::operator()(const char* name, const T& value, TT default_value_maker)
{
    return operator()(name, value, default_value_maker());
}

template<class T>
void dump(std::ostream& str, const T& value)
{
    rapidjson::Document doc;
    adapter<T>::set(doc.GetAllocator(), doc, value);
    rapidjson::OStreamWrapper strw(str);
    rapidjson::Writer<rapidjson::OStreamWrapper> writer(strw);
    doc.Accept(writer);
}

template<class T>
std::string dumps(const T& value)
{
    rapidjson::Document doc;
    adapter<T>::set(doc.GetAllocator(), doc, value);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return { buffer.GetString(), buffer.GetSize() };
}

template<class Func>
class dto_wrapper
{
    Func _func;
public:
    explicit dto_wrapper(Func&& func) : _func(std::move(func)) {}
    explicit dto_wrapper(const Func& func) : _func(func) {}
    explicit dto_wrapper(const dto_wrapper&) = default;
    explicit dto_wrapper(dto_wrapper&&) = default;
    void serialization(auto& io) { _func(io); }
};

template<class Func>
dto_wrapper<std::remove_cvref_t<Func>> wrap(Func&& func)
{
    return { std::forward<Func>(func) };
}

struct init_reader
{
    auto& operator()(const char*) const { return *this; }
    auto& operator()(const char*, auto&) const { return *this; }
    auto& operator()(const char*, io_supported<init_reader> auto& value) const
    {
        value.serialization(*this);
        return *this;
    }
    template<class T, std::convertible_to<T> TT>
    auto& operator()(const char*, T& value, const TT& default_value) const
    {
        value = static_cast<T>(default_value);
        return *this;
    }
    template<class T, class TT>
        requires std::is_convertible_v<std::invoke_result_t<TT>, T>
    auto& operator()(const char*, T& value, TT default_value_maker) const
    {
        value = static_cast<T>(default_value_maker());
        return *this;
    }
};

void init(io_supported<init_reader> auto& value)
{
    init_reader reader{};
    value.serialization(reader);
}

template<class IO>
bool is_reading(IO&)
{
    return std::is_const_v<IO>;
}

template<class DTO>
class as_json
{
    const DTO& _dto;
public:
    explicit as_json(const DTO& dto) : _dto(dto) {}

    friend std::ostream& operator<< (std::ostream& str, const as_json<DTO>& json)
    {
        dump(str, json._dto);
        return str;
    }
};
}
