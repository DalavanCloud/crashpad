// Copyright 2014 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/net/http_multipart_builder.h"

#include <vector>

#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/stringprintf.h"
#include "util/net/http_body.h"

namespace crashpad {

namespace {

const char kCRLF[] = "\r\n";

const char kBoundaryCRLF[] = "\r\n\r\n";

// Generates a random string suitable for use as a multipart boundary.
std::string GenerateBoundaryString() {
  // RFC 2046 §5.1.1 says that the boundary string may be 1 to 70 characters
  // long, choosing from the set of alphanumeric characters along with
  // characters from the set “'()+_,-./:=? ”, and not ending in a space.
  // However, some servers have been observed as dealing poorly with certain
  // nonalphanumeric characters. See
  // blink/Source/platform/network/FormDataBuilder.cpp
  // blink::FormDataBuilder::generateUniqueBoundaryString().
  //
  // This implementation produces a 56-character string with over 190 bits of
  // randomness (62^32 > 2^190).
  std::string boundary_string = "---MultipartBoundary-";
  for (int index = 0; index < 32; ++index) {
    const char kCharacters[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    int random_value = base::RandGenerator(strlen(kCharacters));
    boundary_string += kCharacters[random_value];
  }
  boundary_string += "---";
  return boundary_string;
}

// Escapes the specified name to be suitable for the name field of a
// form-data part.
std::string EncodeMIMEField(const std::string& name) {
  // RFC 2388 §3 says to encode non-ASCII field names according to RFC 2047, but
  // no browsers implement that behavior. Instead, they send field names in the
  // page hosting the form’s encoding. However, some form of escaping is needed.
  // This URL-escapes the quote character and newline characters, per Blink. See
  // blink/Source/platform/network/FormDataBuilder.cpp
  // blink::appendQuotedString().
  //
  // TODO(mark): This encoding is not necessarily correct, and the same code in
  // Blink is marked with a FIXME. Blink does not escape the '%' character,
  // that’s a local addition, but it seems appropriate to be able to decode the
  // string properly.
  std::string encoded;
  for (char character : name) {
    switch (character) {
      case '\r':
      case '\n':
      case '"':
      case '%':
        encoded += base::StringPrintf("%%%02x", character);
        break;
      default:
        encoded += character;
        break;
    }
  }

  return encoded;
}

// Returns a string, formatted with a multipart boundary and a field name,
// after which the contents of the part at |name| can be appended.
std::string GetFormDataBoundary(const std::string& boundary,
                                const std::string& name) {
  return base::StringPrintf(
      "--%s%sContent-Disposition: form-data; name=\"%s\"",
      boundary.c_str(),
      kCRLF,
      EncodeMIMEField(name).c_str());
}

void AssertSafeMIMEType(const std::string& string) {
  for (size_t i = 0; i < string.length(); ++i) {
    char c = string[i];
    CHECK((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '/' ||
          c == '.' ||
          c == '_' ||
          c == '+' ||
          c == '-');
  }
}

}  // namespace

HTTPMultipartBuilder::HTTPMultipartBuilder()
    : boundary_(GenerateBoundaryString()), form_data_(), file_attachments_() {
}

HTTPMultipartBuilder::~HTTPMultipartBuilder() {
}

void HTTPMultipartBuilder::SetFormData(const std::string& key,
                                       const std::string& value) {
  EraseKey(key);
  form_data_[key] = value;
}

void HTTPMultipartBuilder::SetFileAttachment(
    const std::string& key,
    const std::string& upload_file_name,
    const base::FilePath& path,
    const std::string& content_type) {
  EraseKey(upload_file_name);

  FileAttachment attachment;
  attachment.filename = EncodeMIMEField(upload_file_name);
  attachment.path = path;

  if (content_type.empty()) {
    attachment.content_type = "application/octet-stream";
  } else {
    AssertSafeMIMEType(content_type);
    attachment.content_type = content_type;
  }

  file_attachments_[key] = attachment;
}

scoped_ptr<HTTPBodyStream> HTTPMultipartBuilder::GetBodyStream() {
  // The objects inserted into this vector will be owned by the returned
  // CompositeHTTPBodyStream. Take care to not early-return without deleting
  // this memory.
  std::vector<HTTPBodyStream*> streams;

  for (const auto& pair : form_data_) {
    std::string field = GetFormDataBoundary(boundary(), pair.first);
    field += kBoundaryCRLF;
    field += pair.second;
    field += kCRLF;
    streams.push_back(new StringHTTPBodyStream(field));
  }

  for (const auto& pair : file_attachments_) {
    const FileAttachment& attachment = pair.second;
    std::string header = GetFormDataBoundary(boundary(), pair.first);
    header += base::StringPrintf("; filename=\"%s\"%s",
        attachment.filename.c_str(), kCRLF);
    header += base::StringPrintf("Content-Type: %s%s",
        attachment.content_type.c_str(), kBoundaryCRLF);

    streams.push_back(new StringHTTPBodyStream(header));
    streams.push_back(new FileHTTPBodyStream(attachment.path));
    streams.push_back(new StringHTTPBodyStream(kCRLF));
  }

  streams.push_back(
      new StringHTTPBodyStream("--"  + boundary() + "--" + kCRLF));

  return scoped_ptr<HTTPBodyStream>(new CompositeHTTPBodyStream(streams));
}

void HTTPMultipartBuilder::EraseKey(const std::string& key) {
  auto data_it = form_data_.find(key);
  if (data_it != form_data_.end())
    form_data_.erase(data_it);

  auto file_it = file_attachments_.find(key);
  if (file_it != file_attachments_.end())
    file_attachments_.erase(file_it);
}

}  // namespace crashpad
