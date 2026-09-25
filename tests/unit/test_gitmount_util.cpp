// gitmount unit tests — shared pure helpers (byte order, JSON escaping, time).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include "gitmount.hpp"

using gitmount::byte_less;
using gitmount::iso8601_utc;
using gitmount::json_quote_path;

TEST_CASE("byte_less is memcmp order, no locale", "[util]") {
  // '.' (0x2E) < '/' (0x2F): the git tree order would place "foo.txt"
  // before "foo/" — the pinned order is plain bytes (RFC 0000 §3.1).
  CHECK(byte_less("foo", "foo.txt"));
  CHECK(byte_less("foo.txt", "foo2"));
  CHECK(byte_less(".gitmount.json", "HEAD"));
  CHECK(byte_less("HEAD", "branch"));
  CHECK(byte_less("commit", "commits"));
  CHECK(byte_less("commits", "remote"));
  CHECK(byte_less("remote", "tag"));
  // High bytes compare as unsigned: 0xff > 'z' (0x7a).
  CHECK(byte_less("z", "\xff"));
  CHECK_FALSE(byte_less("\xff", "z"));
  // Prefix rule.
  CHECK(byte_less("", "a"));
  CHECK_FALSE(byte_less("a", "a"));
}

TEST_CASE("json_quote_path keeps valid UTF-8 and ASCII", "[util]") {
  CHECK(json_quote_path("/plain/path.git") == "\"/plain/path.git\"");
  CHECK(json_quote_path("/\xe4\xb8\xad\xe6\x96\x87") == "\"/\xe4\xb8\xad\xe6\x96\x87\"");
  // JSON specials are escaped.
  CHECK(json_quote_path("a\"b") == "\"a\\\"b\"");
  CHECK(json_quote_path("a\\b") == "\"a\\\\b\"");
}

TEST_CASE("json_quote_path %XX-escapes invalid UTF-8 bytes", "[util]") {
  // A bare 0xff start byte and a truncated 2-byte sequence.
  CHECK(json_quote_path("/\xff") == "\"/%FF\"");
  CHECK(json_quote_path("/\xc3") == "\"/%C3\"");
  // Overlong encoding is invalid too.
  CHECK(json_quote_path("\xc0\x80") == "\"%C0%80\"");
  // Mix: valid + invalid adjacent.
  CHECK(json_quote_path("a\xffz") == "\"a%FFz\"");
}

TEST_CASE("iso8601_utc formats Zulu time", "[util]") {
  CHECK(iso8601_utc(0) == "1970-01-01T00:00:00Z");
  CHECK(iso8601_utc(1727139600) == "2024-09-24T01:00:00Z");
  CHECK(iso8601_utc(951782400) == "2000-02-29T00:00:00Z");  // leap day
}
