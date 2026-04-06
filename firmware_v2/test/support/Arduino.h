#pragma once

#include <cmath>
#include <cstdint>
#include <ostream>
#include <string>
#include <utility>

class String {
 public:
  String() = default;
  String(const char *value) : value_(value ? value : "") {}
  String(const std::string &value) : value_(value) {}
  String(std::string &&value) : value_(std::move(value)) {}
  String(char value) : value_(1, value) {}
  String(int value) : value_(std::to_string(value)) {}
  String(unsigned int value) : value_(std::to_string(value)) {}
  String(long value) : value_(std::to_string(value)) {}
  String(unsigned long value) : value_(std::to_string(value)) {}

  bool isEmpty() const {
    return value_.empty();
  }

  const char *c_str() const {
    return value_.c_str();
  }

  size_t length() const {
    return value_.length();
  }

  String &operator=(const char *value) {
    value_ = value ? value : "";
    return *this;
  }

  String &operator+=(const String &other) {
    value_ += other.value_;
    return *this;
  }

  String &operator+=(const char *other) {
    value_ += other ? other : "";
    return *this;
  }

  friend bool operator==(const String &lhs, const String &rhs) {
    return lhs.value_ == rhs.value_;
  }

  friend bool operator!=(const String &lhs, const String &rhs) {
    return !(lhs == rhs);
  }

  friend bool operator==(const String &lhs, const char *rhs) {
    return lhs.value_ == (rhs ? rhs : "");
  }

  friend bool operator!=(const String &lhs, const char *rhs) {
    return !(lhs == rhs);
  }

  friend bool operator==(const char *lhs, const String &rhs) {
    return rhs == lhs;
  }

  friend bool operator!=(const char *lhs, const String &rhs) {
    return !(rhs == lhs);
  }

  friend String operator+(const String &lhs, const String &rhs) {
    return String(lhs.value_ + rhs.value_);
  }

  friend String operator+(const String &lhs, const char *rhs) {
    return String(lhs.value_ + (rhs ? rhs : ""));
  }

  friend String operator+(const char *lhs, const String &rhs) {
    return String(std::string(lhs ? lhs : "") + rhs.value_);
  }

  friend std::ostream &operator<<(std::ostream &stream, const String &value) {
    stream << value.value_;
    return stream;
  }

 private:
  std::string value_;
};
