#ifndef INSTRUMENTS_NSOBJECT_H
#define INSTRUMENTS_NSOBJECT_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace instruments {

// NSObject - variant value type representing plist-compatible values.
// Used for NSKeyedArchiver encoding/decoding and DTX message payloads.
class NSObject {
public:
    enum class Type {
        Null,
        Bool,
        Int32,
        Int64,
        UInt64,
        Float32,
        Float64,
        String,
        Data,       // raw bytes
        Array,
        Dictionary,
        Set,
    };

    using ArrayType = std::vector<NSObject>;
    using DictType = std::map<std::string, NSObject>;

    // Constructors
    NSObject() : m_type(Type::Null) {}
    explicit NSObject(bool v) : m_type(Type::Bool), m_bool(v) {}
    explicit NSObject(int32_t v) : m_type(Type::Int32), m_int64(v) {}
    explicit NSObject(int64_t v) : m_type(Type::Int64), m_int64(v) {}
    explicit NSObject(uint64_t v) : m_type(Type::UInt64), m_uint64(v) {}
    explicit NSObject(float v) : m_type(Type::Float32), m_float64(v) {}
    explicit NSObject(double v) : m_type(Type::Float64), m_float64(v) {}
    explicit NSObject(const std::string& v) : m_type(Type::String), m_string(v) {}
    explicit NSObject(std::string&& v) : m_type(Type::String), m_string(std::move(v)) {}
    explicit NSObject(const char* v) : m_type(Type::String), m_string(v ? v : "") {}
    explicit NSObject(const std::vector<uint8_t>& v) : m_type(Type::Data), m_data(v) {}
    explicit NSObject(std::vector<uint8_t>&& v) : m_type(Type::Data), m_data(std::move(v)) {}
    explicit NSObject(const ArrayType& v) : m_type(Type::Array), m_array(v) {}
    explicit NSObject(ArrayType&& v) : m_type(Type::Array), m_array(std::move(v)) {}
    explicit NSObject(const DictType& v) : m_type(Type::Dictionary), m_dict(v) {}
    explicit NSObject(DictType&& v) : m_type(Type::Dictionary), m_dict(std::move(v)) {}

    // Named constructors for disambiguation
    static NSObject Null() { return NSObject(); }
    static NSObject Set(ArrayType items) {
        NSObject obj;
        obj.m_type = Type::Set;
        obj.m_array = std::move(items);
        return obj;
    }
    static NSObject MakeDict(DictType dict) {
        return NSObject(std::move(dict));
    }

    // Type query
    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsInt() const { return m_type == Type::Int32 || m_type == Type::Int64; }
    bool IsUInt() const { return m_type == Type::UInt64; }
    bool IsFloat() const { return m_type == Type::Float32 || m_type == Type::Float64; }
    bool IsString() const { return m_type == Type::String; }
    bool IsData() const { return m_type == Type::Data; }
    bool IsArray() const { return m_type == Type::Array || m_type == Type::Set; }
    bool IsDict() const { return m_type == Type::Dictionary; }

    // Value accessors
    bool AsBool() const { return m_bool; }
    int32_t AsInt32() const { return static_cast<int32_t>(m_int64); }
    int64_t AsInt64() const { return m_int64; }
    uint64_t AsUInt64() const { return m_uint64; }
    float AsFloat() const { return static_cast<float>(m_float64); }
    double AsDouble() const { return m_float64; }
    const std::string& AsString() const { return m_string; }
    const std::vector<uint8_t>& AsData() const { return m_data; }
    const ArrayType& AsArray() const { return m_array; }
    ArrayType& AsArray() { return m_array; }
    const DictType& AsDict() const { return m_dict; }
    DictType& AsDict() { return m_dict; }

    // Numeric conversion (best-effort)
    double ToNumber() const {
        switch (m_type) {
            case Type::Bool:    return m_bool ? 1.0 : 0.0;
            case Type::Int32:
            case Type::Int64:   return static_cast<double>(m_int64);
            case Type::UInt64:  return static_cast<double>(m_uint64);
            case Type::Float32:
            case Type::Float64: return m_float64;
            default: return 0.0;
        }
    }

    // Dictionary helpers
    bool HasKey(const std::string& key) const {
        return m_type == Type::Dictionary && m_dict.count(key) > 0;
    }

    const NSObject& operator[](const std::string& key) const {
        static NSObject null;
        if (m_type != Type::Dictionary) return null;
        auto it = m_dict.find(key);
        return it != m_dict.end() ? it->second : null;
    }

    NSObject& operator[](const std::string& key) {
        if (m_type != Type::Dictionary) {
            m_type = Type::Dictionary;
        }
        return m_dict[key];
    }

    // Array helpers
    size_t Size() const {
        if (IsArray()) return m_array.size();
        if (IsDict()) return m_dict.size();
        return 0;
    }

    void Append(NSObject value) {
        if (m_type == Type::Array || m_type == Type::Set) {
            m_array.push_back(std::move(value));
        }
    }

    // JSON serialization for debugging
    std::string ToJson(int indent = 0) const;

    // Class metadata for NSKeyedArchiver
    void SetClassName(const std::string& name) { m_className = name; }
    void SetClassHierarchy(const std::vector<std::string>& hierarchy) { m_classHierarchy = hierarchy; }
    const std::string& ClassName() const { return m_className; }
    const std::vector<std::string>& ClassHierarchy() const { return m_classHierarchy; }

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    int64_t m_int64 = 0;
    uint64_t m_uint64 = 0;
    double m_float64 = 0.0;
    std::string m_string;
    std::vector<uint8_t> m_data;
    ArrayType m_array;
    DictType m_dict;

    // NSKeyedArchiver class metadata
    std::string m_className;
    std::vector<std::string> m_classHierarchy;
};

} // namespace instruments

#endif // INSTRUMENTS_NSOBJECT_H
