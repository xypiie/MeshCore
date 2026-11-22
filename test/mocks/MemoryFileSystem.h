#pragma once

#include "Stream.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

class File : public Stream {
  std::shared_ptr<std::vector<uint8_t>> _bytes;
  size_t _position = 0;

public:
  File() = default;
  explicit File(std::shared_ptr<std::vector<uint8_t>> bytes) : _bytes(bytes) {}
  explicit operator bool() const { return static_cast<bool>(_bytes); }
  int available() override { return _bytes ? _bytes->size() - _position : 0; }
  int read() override { return available() ? (*_bytes)[_position++] : -1; }
  size_t read(uint8_t* bytes, size_t count) {
    count = std::min(count, static_cast<size_t>(available()));
    if (count) std::copy_n(_bytes->data() + _position, count, bytes);
    _position += count;
    return count;
  }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (!_bytes) return 0;
    if (_position + count > _bytes->size()) _bytes->resize(_position + count);
    std::copy_n(bytes, count, _bytes->begin() + _position);
    _position += count;
    return count;
  }
  void close() {}
};

class MemoryFileSystem {
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> _files;

public:
  bool exists(const char* path) const { return _files.count(path) != 0; }
  bool remove(const char* path) { return _files.erase(path) != 0; }
  bool mkdir(const char*) { return true; }
  File open(const char* path, const char* mode = "r", bool = false) {
    if (mode[0] == 'w') _files[path] = std::make_shared<std::vector<uint8_t>>();
    auto file = _files.find(path);
    return file == _files.end() ? File() : File(file->second);
  }
};
