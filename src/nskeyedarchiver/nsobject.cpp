#include "nsobject.h"
#include <cstdio>
#include <sstream>

namespace instruments {

static void AppendIndent(std::ostringstream& ss, int indent) {
    for (int i = 0; i < indent; i++) ss << "  ";
}

static std::string EscapeJson(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

std::string NSObject::ToJson(int indent) const {
    std::ostringstream ss;

    switch (m_type) {
        case Type::Null:
            ss << "null";
            break;

        case Type::Bool:
            ss << (m_bool ? "true" : "false");
            break;

        case Type::Int32:
        case Type::Int64:
            ss << m_int64;
            break;

        case Type::UInt64:
            ss << m_uint64;
            break;

        case Type::Float32:
        case Type::Float64:
            ss << m_float64;
            break;

        case Type::String:
            ss << "\"" << EscapeJson(m_string) << "\"";
            break;

        case Type::Data: {
            ss << "\"<data:" << m_data.size() << " bytes>\"";
            break;
        }

        case Type::Array:
        case Type::Set: {
            if (m_array.empty()) {
                ss << "[]";
                break;
            }
            ss << "[\n";
            for (size_t i = 0; i < m_array.size(); i++) {
                AppendIndent(ss, indent + 1);
                ss << m_array[i].ToJson(indent + 1);
                if (i + 1 < m_array.size()) ss << ",";
                ss << "\n";
            }
            AppendIndent(ss, indent);
            ss << "]";
            break;
        }

        case Type::Dictionary: {
            if (m_dict.empty()) {
                ss << "{}";
                break;
            }
            ss << "{\n";
            size_t count = 0;
            for (const auto& [key, val] : m_dict) {
                AppendIndent(ss, indent + 1);
                ss << "\"" << EscapeJson(key) << "\": " << val.ToJson(indent + 1);
                if (++count < m_dict.size()) ss << ",";
                ss << "\n";
            }
            AppendIndent(ss, indent);
            ss << "}";
            break;
        }
    }

    return ss.str();
}

} // namespace instruments
