#include "ferryman/util/StringUtil.hpp"

#include <array>
#include <cctype>
#include <sstream>

namespace ferryman::util {

namespace {

char DecodeNibble(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<char>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<char>(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<char>(c - 'A' + 10);
  }
  return 0;
}

std::string ParseJsonString(std::string_view json, size_t& cursor) {
  std::string out;
  if (cursor >= json.size() || json[cursor] != '"') {
    return out;
  }
  ++cursor;
  while (cursor < json.size()) {
    const char c = json[cursor++];
    if (c == '"') {
      break;
    }
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (cursor >= json.size()) {
      break;
    }
    const char esc = json[cursor++];
    switch (esc) {
      case '"':
      case '\\':
      case '/':
        out.push_back(esc);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u':
        if (cursor + 3 < json.size()) {
          cursor += 4;
        }
        break;
      default:
        out.push_back(esc);
        break;
    }
  }
  return out;
}

void SkipSpaces(std::string_view json, size_t& cursor) {
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor]))) {
    ++cursor;
  }
}

}  // namespace

std::string Trim(std::string_view value) {
  size_t start = 0;
  size_t end = value.size();
  while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return std::string(value.substr(start, end - start));
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream out;
  for (const char c : value) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out << "\\u00";
          const unsigned char uc = static_cast<unsigned char>(c);
          static constexpr std::array<char, 16> kHex{
              '0', '1', '2', '3', '4', '5', '6', '7',
              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
          out << kHex[(uc >> 4) & 0x0f] << kHex[uc & 0x0f];
        } else {
          out << c;
        }
        break;
    }
  }
  return out.str();
}

std::string Base64Encode(std::string_view data) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);

  size_t index = 0;
  while (index + 2 < data.size()) {
    const unsigned int n =
        (static_cast<unsigned char>(data[index]) << 16) |
        (static_cast<unsigned char>(data[index + 1]) << 8) |
        static_cast<unsigned char>(data[index + 2]);
    out.push_back(kTable[(n >> 18) & 63]);
    out.push_back(kTable[(n >> 12) & 63]);
    out.push_back(kTable[(n >> 6) & 63]);
    out.push_back(kTable[n & 63]);
    index += 3;
  }

  if (index < data.size()) {
    unsigned int n = static_cast<unsigned char>(data[index]) << 16;
    out.push_back(kTable[(n >> 18) & 63]);
    if (index + 1 < data.size()) {
      n |= static_cast<unsigned char>(data[index + 1]) << 8;
      out.push_back(kTable[(n >> 12) & 63]);
      out.push_back(kTable[(n >> 6) & 63]);
      out.push_back('=');
    } else {
      out.push_back(kTable[(n >> 12) & 63]);
      out.push_back('=');
      out.push_back('=');
    }
  }

  return out;
}

std::string Base64Decode(std::string_view encoded) {
  static constexpr std::array<int, 256> kIndex = [] {
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 26; ++i) {
      table[static_cast<size_t>('A' + i)] = i;
      table[static_cast<size_t>('a' + i)] = i + 26;
    }
    for (int i = 0; i < 10; ++i) {
      table[static_cast<size_t>('0' + i)] = i + 52;
    }
    table[static_cast<size_t>('+')] = 62;
    table[static_cast<size_t>('/')] = 63;
    return table;
  }();

  std::string out;
  int val = 0;
  int bits = -8;
  for (const unsigned char c : encoded) {
    if (std::isspace(c)) {
      continue;
    }
    if (c == '=') {
      break;
    }
    const int decoded = kIndex[c];
    if (decoded < 0) {
      continue;
    }
    val = (val << 6) + decoded;
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xff));
      bits -= 8;
    }
  }
  return out;
}

std::unordered_map<std::string, std::string> ParseFlatJsonObject(std::string_view json) {
  std::unordered_map<std::string, std::string> result;
  size_t cursor = 0;
  SkipSpaces(json, cursor);
  if (cursor >= json.size() || json[cursor] != '{') {
    return result;
  }
  ++cursor;
  while (cursor < json.size()) {
    SkipSpaces(json, cursor);
    if (cursor < json.size() && json[cursor] == '}') {
      ++cursor;
      break;
    }
    std::string key = ParseJsonString(json, cursor);
    SkipSpaces(json, cursor);
    if (cursor >= json.size() || json[cursor] != ':') {
      break;
    }
    ++cursor;
    SkipSpaces(json, cursor);

    std::string value;
    if (cursor < json.size() && json[cursor] == '"') {
      value = ParseJsonString(json, cursor);
    } else {
      const size_t start = cursor;
      while (cursor < json.size() && json[cursor] != ',' && json[cursor] != '}') {
        ++cursor;
      }
      value = Trim(json.substr(start, cursor - start));
    }
    result[key] = value;

    SkipSpaces(json, cursor);
    if (cursor < json.size() && json[cursor] == ',') {
      ++cursor;
    }
  }
  return result;
}

std::string BuildJsonObject(const std::vector<JsonField>& fields) {
  std::ostringstream out;
  out << '{';
  for (size_t i = 0; i < fields.size(); ++i) {
    const JsonField& field = fields[i];
    out << '"' << JsonEscape(field.key) << '"' << ':';
    if (field.raw_value) {
      out << field.value;
    } else {
      out << '"' << JsonEscape(field.value) << '"';
    }
    if (i + 1 < fields.size()) {
      out << ',';
    }
  }
  out << '}';
  return out.str();
}

}  // namespace ferryman::util
