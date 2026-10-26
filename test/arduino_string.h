// A stand-in for Arduino's String, for compiling tt_text.h on a computer. Only what
// tt_text.h uses, with Arduino's behavior (indexes are int, -1 = not found; substring
// clamps; trim strips whitespace at both ends).
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

class String {
 public:
  String() {}
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  String(char c) : s_(1, c) {}

  unsigned int length() const { return s_.size(); }
  const char* c_str() const { return s_.c_str(); }
  char operator[](unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
  void setCharAt(unsigned int i, char c) { if (i < s_.size()) s_[i] = c; }

  int indexOf(char c, unsigned int from = 0) const { return pos(s_.find(c, from)); }
  int indexOf(const char* t, unsigned int from = 0) const { return pos(s_.find(t, from)); }
  int indexOf(const String& t, unsigned int from = 0) const { return pos(s_.find(t.s_, from)); }
  int lastIndexOf(char c) const { return pos(s_.rfind(c)); }
  int lastIndexOf(const char* t) const { return pos(s_.rfind(t)); }

  String substring(unsigned int from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
  String substring(unsigned int from, unsigned int to) const {
    if (to > s_.size()) to = s_.size();
    if (from >= to) return String();
    return String(s_.substr(from, to - from));
  }

  void replace(char a, char b) { for (auto& c : s_) if (c == a) c = b; }
  void replace(const char* a, const char* b) {
    size_t n = strlen(a), at = 0;
    if (!n) return;
    while ((at = s_.find(a, at)) != std::string::npos) { s_.replace(at, n, b); at += strlen(b); }
  }
  void remove(unsigned int from) { if (from < s_.size()) s_.erase(from); }
  void remove(unsigned int from, unsigned int n) { if (from < s_.size()) s_.erase(from, n); }
  void trim() {
    size_t a = s_.find_first_not_of(" \t\r\n"), b = s_.find_last_not_of(" \t\r\n");
    s_ = a == std::string::npos ? std::string() : s_.substr(a, b - a + 1);
  }
  long toInt() const { return atol(s_.c_str()); }
  bool startsWith(const String& t) const { return s_.compare(0, t.s_.size(), t.s_) == 0; }
  bool endsWith(const String& t) const {
    return s_.size() >= t.s_.size() && s_.compare(s_.size() - t.s_.size(), t.s_.size(), t.s_) == 0;
  }

  String& operator+=(char c) { s_ += c; return *this; }
  String& operator+=(const char* t) { s_ += t; return *this; }
  String& operator+=(const String& t) { s_ += t.s_; return *this; }
  friend String operator+(String a, const String& b) { a += b; return a; }
  friend String operator+(String a, const char* b) { a += b; return a; }
  bool operator==(const String& t) const { return s_ == t.s_; }
  bool operator==(const char* t) const { return s_ == t; }
  bool operator!=(const String& t) const { return s_ != t.s_; }
  bool operator!=(const char* t) const { return s_ != t; }

  const std::string& str() const { return s_; }

 private:
  static int pos(size_t p) { return p == std::string::npos ? -1 : (int)p; }
  std::string s_;
};
