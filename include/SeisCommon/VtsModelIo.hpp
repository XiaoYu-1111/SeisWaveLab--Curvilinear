#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <istream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Bounded ASCII VTS model metadata/stream parser shared by the independent 2D and 3D modules.
namespace SeisWave::VtsModel {
using Arrays = std::map<std::string, std::vector<double>>;
inline void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Checksum {
    std::uint64_t value{14695981039346656037ULL};
    void byte(unsigned char b) { value = (value ^ b) * 1099511628211ULL; }
    void name(const std::string &s) {
        for (unsigned char c : s)
            byte(c);
        byte(0);
    }
    void number(double d) {
        // Canonical bytes, independent of host endian and signed-zero formatting.
        const auto bits = std::bit_cast<std::uint64_t>(d == 0 ? 0.0 : d);
        for (int shift = 0; shift < 64; shift += 8)
            byte(static_cast<unsigned char>(bits >> shift));
    }
};
inline std::string attribute(const std::string &tag, const std::string &name) {
    const auto pos = tag.find(name + "=");
    require(pos != std::string::npos, "Missing VTS attribute: " + name);
    const auto start = pos + name.size() + 1;
    require(start < tag.size() && (tag[start] == '\"' || tag[start] == '\''), "Invalid VTS attribute.");
    const auto end = tag.find(tag[start], start + 1);
    require(end != std::string::npos, "Unterminated VTS attribute.");
    return tag.substr(start + 1, end - start - 1);
}
inline std::string tag(std::istream &in) {
    in >> std::ws;
    require(in.get() == '<', "Expected VTS XML tag (truncated or unsupported file).");
    std::string s{"<"};
    for (int i = 0; i < 8192; ++i) {
        const auto c = in.get();
        require(c != std::char_traits<char>::eof(), "Truncated VTS XML tag.");
        s.push_back(static_cast<char>(c));
        if (c == '>')
            return s;
    }
    throw std::runtime_error("Oversized VTS XML tag.");
}
inline void expected(std::istream &in, const char *name) {
    require(tag(in) == name, std::string("Expected ") + name);
}
inline int integer(double value, int low, int high) {
    require(std::isfinite(value) && value >= low && value <= high && value == std::floor(value),
            "Invalid integer model parameter.");
    return static_cast<int>(value);
}
// Stream large point arrays; only compact metadata is retained in memory.
inline std::vector<double> readArray(std::istream &in, const std::string &header, Checksum *hash,
                                     std::size_t count, bool retain,bool allowFloat32=false) {
    const bool fp32=attribute(header,"type")=="Float32";
    require(attribute(header, "format") == "ascii" && (attribute(header, "type") == "Float64"||(allowFloat32&&fp32)),
            "Model import requires supported SeisWave ASCII floating point VTS arrays.");
    const auto name = attribute(header, "Name");
    if (hash)
        hash->name(name);
    std::vector<double> values;
    if (retain)
        values.reserve(count);
    for (std::size_t n = 0; n < count; ++n) {
        double v{};
        require(static_cast<bool>(in >> v) && std::isfinite(v), "Missing or nonfinite VTS data: " + name);
        if(fp32){v=static_cast<float>(v);require(std::isfinite(v),"Float32 VTS value overflows: "+name);}
        if (hash)
            hash->number(v);
        if (retain)
            values.push_back(v);
    }
    expected(in, "</DataArray>");
    return values;
}
} // namespace SeisWave::VtsModel
