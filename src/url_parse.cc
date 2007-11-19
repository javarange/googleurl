/* Based on nsURLParsers.cc from Mozilla
 * -------------------------------------
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Darin Fisher (original author)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "googleurl/src/url_parse.h"

#include "base/logging.h"
#include "googleurl/src/url_parse_internal.h"

namespace url_parse {

namespace {

// Returns true if the given character is a valid digit to use in a port.
inline bool IsPortDigit(UTF16Char ch) {
  return ch >= '0' && ch <= '9';
}

// Returns the offset of the next authority terminator in the input starting
// from start_offset. If no terminator is found, the return value will be equal
// to spec_len.
template<typename CHAR>
int FindNextAuthorityTerminator(const CHAR* spec,
                                int start_offset,
                                int spec_len) {
  for (int i = start_offset; i < spec_len; i++) {
    if (IsAuthorityTerminator(spec[i]))
      return i;
  }
  return spec_len;  // Not found.
}

// Fills in all members of the Parsed structure except for the scheme.
//
// |spec| is the full spec being parsed, of length |spec_len|.
// |after_scheme| is the character immediately following the scheme (after the
//   colon) where we'll begin parsing.
//
// Compatability data points. I list "host", "path" extracted:
// Input                IE6             Firefox                Us
// -----                --------------  --------------         --------------
// http://foo.com/      "foo.com", "/"  "foo.com", "/"         "foo.com", "/"                      
// http:foo.com/        "foo.com", "/"  "foo.com", "/"         "foo.com", "/"
// http:/foo.com/       fail(*)         "foo.com", "/"         "foo.com", "/"
// http:\foo.com/       fail(*)         "\foo.com", "/"(fail)  "foo.com", "/"
// http:////foo.com/    "foo.com", "/"  "foo.com", "/"         "foo.com", "/"
//
// (*) Interestingly, although IE fails to load these URLs, its history
// canonicalizer handles them, meaning if you've been to the corresponding
// "http://foo.com/" link, it will be colored.
template <typename CHAR>
void DoParseAfterScheme(const CHAR* spec,
                        int spec_len,
                        int after_scheme,
                        Parsed* parsed) {
  int num_slashes = CountConsecutiveSlashes(spec, after_scheme, spec_len);
  int after_slashes = after_scheme + num_slashes;

  // First split into two main parts, the authority (username, password, host,
  // and port) and the full path (path, query, and reference).
  Component authority;
  Component full_path;

  // Found "//<some data>", looks like an authority section. Treat everything
  // from there to the next slash (or end of spec) to be the authority. Note
  // that we ignore the number of slashes and treat it as the authority.
  int end_auth = FindNextAuthorityTerminator(spec, after_slashes, spec_len);
  authority = Component(after_slashes, end_auth - after_slashes);

  if (end_auth == spec_len)  // No beginning of path found.
    full_path = Component();
  else  // Everything starting from the slash to the end is the path.
    full_path = Component(end_auth, spec_len - end_auth);

  // Now parse those two sub-parts.
  ParseAuthority(spec, authority, &parsed->username, &parsed->password,
                 &parsed->host, &parsed->port);
  ParsePath(spec, full_path, &parsed->path, &parsed->query, &parsed->ref);
}

template<typename CHAR>
void ParseUserInfo(const CHAR* spec,
                   const Component& user,
                   Component* username,
                   Component* password) {
  // Find the first colon in the user section, which separates the username and
  // password.
  int colon_offset = 0;
  while (colon_offset < user.len && spec[user.begin + colon_offset] != ':')
    colon_offset++;

  if (colon_offset < user.len) {
    // Found separator: <username>:<password>
    *username = Component(user.begin, colon_offset);
    *password = MakeRange(user.begin + colon_offset + 1,
                          user.begin + user.len);
  } else {
    // No separator, treat everything as the username
    *username = user;
    *password = Component();
  }
}

template<typename CHAR>
void ParseServerInfo(const CHAR* spec,
                     const Component& serverinfo,
                     Component* hostname,
                     Component* port_num) {
  if (serverinfo.len == 0) {
    // No server info, host name is empty.
    *hostname = Component(serverinfo.begin, 0);
    *port_num = Component();
    return;
  }

  // Search backwards for a ':' but stop on ']' (IPv6 address literal
  // delimiter).
  int i = serverinfo.begin + serverinfo.len - 1;
  int colon = -1, bracket = -1;
  while (i >= serverinfo.begin && colon < 0) {
    switch (spec[i]) {
      case ']':
        bracket = i;
        break;
      case ':':
        if (bracket < 0)
          colon = i;  // Will cause loop to terminate.
        break;
    }
    i--;
  }

  if (colon >= 0) {
    // Found a port number: <hostname>:<port>
    *hostname = MakeRange(serverinfo.begin, colon);
    *port_num = MakeRange(colon + 1, serverinfo.begin + serverinfo.len);
  } else {
    // No port: <hostname>
    *hostname = serverinfo;
    *port_num = Component();
  }
}

// Given an already-identified auth section, breaks it into its consituent
// parts. The port number will be parsed and the resulting integer will be
// filled into the given *port variable, or -1 if there is no port number or it
// is invalid.
template<typename CHAR>
void ParseAuthority(const CHAR* spec,
                    const Component& auth,
                    Component* username,
                    Component* password,
                    Component* hostname,
                    Component* port_num) {
  DCHECK(auth.is_valid()) << "We should always get an authority";
  if (auth.len == 0) {
    *username = Component();
    *password = Component();
    *hostname = Component(0, 0);
    *port_num = Component();
    return;
  }

  // Search backwards for @, which is the separator between the user info and
  // the server info.
  int i = auth.begin + auth.len - 1;
  while (i > auth.begin && spec[i] != '@')
    i--;

  if (spec[i] == '@') {
    // Found user info: <user-info>@<server-info>
    ParseUserInfo(spec, Component(auth.begin, i - auth.begin),
                  username, password);
    ParseServerInfo(spec, MakeRange(i + 1, auth.begin + auth.len),
                    hostname, port_num);
  } else {
    // No user info, everything is server info.
    *username = Component();
    *password = Component();
    ParseServerInfo(spec, auth, hostname, port_num);
  }
}

template<typename CHAR>
void ParsePath(const CHAR* spec,
               const Component& path,
               Component* filepath,
               Component* query,
               Component* ref) {
  // path = [/]<segment1>/<segment2>/<...>/<segmentN>;<param>?<query>#<ref>

  // Special case when there is no path.
  if (path.len == -1) {
    *filepath = Component();
    *query = Component();
    *ref = Component();
    return;
  }
  DCHECK(path.len > 0) << "We should never have 0 length paths";

  // Search for first occurrence of either ? or #.
  int path_end = path.begin + path.len;

  int query_separator = -1;  // Index of the '?'
  int ref_separator = -1;    // Index of the '#'
  for (int i = path.begin; i < path_end; i++) {
    switch (spec[i]) {
      case '?':
        // Only match the query string if it precedes the reference fragment
        // and when we haven't found one already.
        if (ref_separator < 0 && query_separator < 0)
          query_separator = i;
        break;
      case '#':
        // We want to find the LAST reference fragment, so overwrite any
        // previous one.
        ref_separator = i;
        break;
    }
  }

  // Markers pointing to the character after each of these corresponding
  // components. The code below words from the end back to the beginning,
  // and will update these indices as it finds components that exist.
  int file_end, query_end;

  // Ref fragment: from the # to the end of the path.
  if (ref_separator >= 0) {
    file_end = query_end = ref_separator;
    *ref = MakeRange(ref_separator + 1, path_end);
  } else {
    file_end = query_end = path_end;
    *ref = Component();
  }

  // Query fragment: everything from the ? to the next boundary (either the end
  // of the path or the ref fragment).
  if (query_separator >= 0) {
    file_end = query_separator;
    *query = MakeRange(query_separator + 1, query_end);
  } else {
    *query = Component();
  }

  // File path: treat an empty file path as no file path.
  if (file_end != path.begin)
    *filepath = MakeRange(path.begin, file_end);
  else
    *filepath = Component();
}

template<typename CHAR>
bool DoExtractScheme(const CHAR* url,
                     int url_len,
                     Component* scheme) {
  // Skip leading whitespace and control characters.
  int begin = 0;
  while (begin < url_len && ShouldTrimFromURL(url[begin]))
    begin++;
  if (begin == url_len)
    return false;  // Input is empty or all whitespace.

  // Find the first colon character.
  for (int i = begin; i < url_len; i++) {
    if (url[i] == ':') {
      *scheme = MakeRange(begin, i);
      return true;
    } else if (IsAuthorityTerminator(url[i])) {
      // An authority terminator was found before the end of the scheme, so we
      // say that there is no scheme (for example "google.com/foo:bar").
      return false;
    }
  }
  return false;  // No colon found: no scheme
}

// The main parsing function for URLs, this is the backend for the 
template<typename CHAR>
void DoParseStandardURL(const CHAR* spec, int spec_len, Parsed* parsed) {
  DCHECK(spec_len >= 0);

  // Strip leading & trailing spaces and control characters.
  int begin = 0;
  TrimURL(spec, &begin, &spec_len);

  // Handle empty specs or ones that contain only whitespace or control chars.
  if (begin == spec_len) {
    // ParsedAfterScheme will fill in empty values if there is no more data.
    parsed->scheme = Component();
    DoParseAfterScheme(spec, spec_len, begin, parsed);
    return;
  }

  // Find the first non-scheme character before the beginning of the path. This
  // code handles URLs that may have empty schemes, which makes it different
  // than the ExtractScheme code above, which can happily fail if it doesn't
  // find a colon.
  int scheme_colon = -1;  // Index of first colon that preceeds the authority
  for (int i = begin; i < spec_len; i++) {
    if (IsAuthorityTerminator(spec[i]) ||
        spec[i] == '@' || spec[i] == '[') {
      // Start of path, found a username ("@"), or start of an IPV6 address
      // literal. This means there is no scheme found.
      break;
    }
    if (spec[i] == ':') {
      scheme_colon = i;
      break;
    }
  }

  int after_scheme;
  if (scheme_colon >= 0) {
    //   spec = <scheme>:/<the-rest>
    // or
    //   spec = <scheme>:<authority>
    //   spec = <scheme>:<path-no-slashes>
    parsed->scheme = MakeRange(begin, scheme_colon);
    after_scheme = scheme_colon + 1;  // Character following the colon.
  } else {
    //   spec = <authority-no-port-or-password>/<path>
    //   spec = <path>
    // or
    //   spec = <authority-no-port-or-password>/<path-with-colon>
    //   spec = <path-with-colon>
    // or
    //   spec = <authority-no-port-or-password>
    //   spec = <path-no-slashes-or-colon>
    parsed->scheme = Component();
    after_scheme = begin;
  }
  DoParseAfterScheme(spec, spec_len, after_scheme, parsed);
}

// Initializes a path URL which is merely a scheme followed by a path. Examples
// include "about:foo" and "javascript:alert('bar');"
template<typename CHAR>
void DoParsePathURL(const CHAR* spec, int spec_len, Parsed* parsed) {
  // Get the non-path and non-scheme parts of the URL out of the way, we never
  // use them.
  parsed->username = Component();
  parsed->password = Component();
  parsed->host = Component(0, 0);
  parsed->port = Component();
  parsed->query = Component();
  parsed->ref = Component();

  // Strip leading & trailing spaces and control characters.
  int begin = 0;
  TrimURL(spec, &begin, &spec_len);

  // Handle empty specs or ones that contain only whitespace or control chars.
  if (begin == spec_len) {
    // ParsedAfterScheme will fill in empty values if there is no more data.
    parsed->scheme = Component();
    parsed->path = Component();
    return;
  }

  // Extract the scheme, with the path being everything following. We also
  // handle the case where there is no scheme.
  if (ExtractScheme(&spec[begin], spec_len - begin, &parsed->scheme)) {
    // Offset the results since we gave ExtractScheme a substring.
    parsed->scheme.begin += begin;

    // For compatability with the standard URL parser, we treat no path as
    // -1, rather than having a length of 0 (we normally wouldn't care so
    // much for these non-standard URLs).
    if (parsed->scheme.end() == spec_len - 1)
      parsed->path = Component();
    else
      parsed->path = MakeRange(parsed->scheme.end() + 1, spec_len);
  } else {
    // No scheme found, just path.
    parsed->scheme = Component();
    parsed->path = MakeRange(begin, spec_len);
  }
}

// Converts a port number in a string to an integer. We'd like to just call
// sscanf but our input is not NULL-terminated, which sscanf requires. Instead,
// we copy the digits to a small stack buffer (since we know the maximum number
// of digits in a valid port number) that we can NULL terminate.
template<typename CHAR>
int DoParsePort(const CHAR* spec, const Component& component) {
  // Easy success case when there is no port.
  const int max_digits = 5;
  if (!component.is_nonempty())
    return PORT_UNSPECIFIED;

  // Skip over any leading 0s.
  Component digits_comp(component.end(), 0);
  for (int i = 0; i < component.len; i++) {
    if (spec[component.begin + i] != '0') {
      digits_comp = MakeRange(component.begin + i, component.end());
      break;
    }
  }
  if (digits_comp.len == 0)
    return 0;  // All digits were 0.

  // Verify we don't have too many digits (we'll be copying to our buffer so
  // we need to double-check).
  if (digits_comp.len > max_digits)
    return PORT_INVALID;

  // Copy valid digits to the buffer.
  char digits[max_digits + 1];  // +1 for null terminator
  for (int i = 0; i < digits_comp.len; i++) {
    CHAR ch = spec[digits_comp.begin + i];
    if (!IsPortDigit(ch)) {
      // Invalid port digit, fail.
      return PORT_INVALID;
    }
    digits[i] = static_cast<char>(ch);
  }

  // Null-terminate the string and convert to integer. Since we guarantee
  // only digits, atoi's lack of error handling is OK.
  digits[digits_comp.len] = 0;
  int port = atoi(digits);
  if (port > 65535)
    return PORT_INVALID;  // Out of range.
  return port;
}

template<typename CHAR>
void DoExtractFileName(const CHAR* spec,
                       const Component& path,
                       Component* file_name) {
  // Handle empty paths: they have no file names.
  if (!path.is_nonempty()) {
    *file_name = Component();
    return;
  }

  // Search backwards for a parameter, which is a normally unused field in a
  // URL delimited by a semicolon. We parse the parameter as part of the
  // path, but here, we don't want to count it. The last semicolon is the
  // parameter. The path should start with a slash, so we don't need to check
  // the first one.
  int file_end = path.end();
  for (int i = path.end() - 1; i > path.begin; i--) {
    if (spec[i] == ';') {
      file_end = i;
      break;
    }
  }

  // Now search backwards from the filename end to the previous slash
  // to find the beginning of the filename.
  for (int i = file_end - 1; i >= path.begin; i--) {
    if (IsURLSlash(spec[i])) {
      // File name is everything following this character to the end
      *file_name = MakeRange(i + 1, file_end);
      return;
    }
  }

  // No slash found, this means the input was degenerate (generally paths
  // will start with a slash). Let's call everything the file name.
  *file_name = MakeRange(path.begin, file_end);
  return;
}

template<typename CHAR>
void DoExtractQueryFragment(const CHAR* spec,
                            Component* query,
                            Component* key,
                            Component* value) {
  int start = query->begin;
  int c = start;
  int end = query->begin + query->len;
  while (c < end && spec[c] != '&' && spec[c] != '=')
    c++;

  if ((c - start) > 0) {
    key->begin = start;
    key->len = c - start;
  }

  // We have a key, skip the separator if any
  if (c < end && spec[c] == '=')
    ++c;

  if (c < end) {
    start = c;
    while (c < end && spec[c] != '&')
      c++;
    if ((c - start) > 0) {
      value->begin = start;
      value->len = c - start;
    }
  }

  // Finally skip the next separator if any
  if (c < end && spec[c] == '&')
    ++c;

  // Save the new query
  query->begin = c;
  query->len = end - c;
}

}  // namespace

int Parsed::Length() const {
  if (ref.is_nonempty())
    return ref.end();
  if (query.is_nonempty())
    return query.end();
  if (path.is_nonempty())
    return path.end();
  if (port.is_nonempty())
    return port.end();
  if (host.is_nonempty())
    return host.end();
  if (password.is_nonempty())
    return password.end();
  if (username.is_nonempty())
    return username.end();
  if (scheme.is_nonempty())
    return scheme.end();
  return 0;
}

bool ExtractScheme(const char* url, int url_len, Component* scheme) {
  return DoExtractScheme(url, url_len, scheme);
}

bool ExtractScheme(const UTF16Char* url, int url_len, Component* scheme) {
  return DoExtractScheme(url, url_len, scheme);
}

// This handles everything that may be an authority terminator, including
// backslash. For special backslash handling see DoParseAfterScheme.
bool IsAuthorityTerminator(UTF16Char ch) {
  return IsURLSlash(ch) || ch == '?' || ch == '#' || ch == ';';
}

void ExtractFileName(const char* url,
                     const Component& path,
                     Component* file_name) {
  DoExtractFileName(url, path, file_name);
}

void ExtractFileName(const UTF16Char* url,
                     const Component& path,
                     Component* file_name) {
  DoExtractFileName(url, path, file_name);
}

void ExtractQueryFragment(const char* url,
                          Component* query,
                          Component* key,
                          Component* value) {
  DoExtractQueryFragment(url, query, key, value);
}

void ExtractQueryFragment(const UTF16Char* url,
                          Component* query,
                          Component* key,
                          Component* value) {
  DoExtractQueryFragment(url, query, key, value);
}

int ParsePort(const char* url, const Component& port) {
  return DoParsePort(url, port);
}

int ParsePort(const UTF16Char* url, const Component& port) {
  return DoParsePort(url, port);
}

void ParseStandardURL(const char* url, int url_len, Parsed* parsed) {
  DoParseStandardURL(url, url_len, parsed);
}

void ParseStandardURL(const UTF16Char* url, int url_len, Parsed* parsed) {
  DoParseStandardURL(url, url_len, parsed);
}

void ParsePathURL(const char* url, int url_len, Parsed* parsed) {
  DoParsePathURL(url, url_len, parsed);
}

void ParsePathURL(const UTF16Char* url, int url_len, Parsed* parsed) {
  DoParsePathURL(url, url_len, parsed);
}

void ParsePathInternal(const char* spec,
                       const Component& path,
                       Component* filepath,
                       Component* query,
                       Component* ref) {
  ParsePath(spec, path, filepath, query, ref);
}

void ParsePathInternal(const UTF16Char* spec,
                       const Component& path,
                       Component* filepath,
                       Component* query,
                       Component* ref) {
  ParsePath(spec, path, filepath, query, ref);
}

void ParseAfterScheme(const char* spec,
                      int spec_len,
                      int after_scheme,
                      Parsed* parsed) {
  DoParseAfterScheme(spec, spec_len, after_scheme, parsed);
}

void ParseAfterScheme(const UTF16Char* spec,
                      int spec_len,
                      int after_scheme,
                      Parsed* parsed) {
  DoParseAfterScheme(spec, spec_len, after_scheme, parsed);
}

}  // namespace url_parse