/*
  Asynchronous WebServer library for Espressif MCUs

  Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  This file is part of the esp8266 core for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "cbuf.h"
#include "ESPAsyncWebServer.h"
#include "WebResponseImpl.h"

#include <unordered_map>

/// Hash function for Arduino String
namespace std {
  template<>
  struct hash<String> {
    size_t operator()(const String& _Keyval) const noexcept { return (std::_Hash_bytes(_Keyval.c_str(), _Keyval.length(), 0)); }
  };
}  // namespace std

// Since ESP8266 does not link memchr by default, here's its implementation.
void* memchr(void* ptr, int ch, size_t count) {
  unsigned char* p = static_cast<unsigned char*>(ptr);
  while (count--)
    if (*p++ == static_cast<unsigned char>(ch)) return --p;
  return nullptr;
}

/*
 * Abstract Response
 * */
const char* AsyncWebServerResponse::_responseCodeToString(int code) {
  switch (code) {
    case 100:
      return "Continue";
    case 101:
      return "Switching Protocols";
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 203:
      return "Non-Authoritative Information";
    case 204:
      return "No Content";
    case 205:
      return "Reset Content";
    case 206:
      return "Partial Content";
    case 300:
      return "Multiple Choices";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 303:
      return "See Other";
    case 304:
      return "Not Modified";
    case 305:
      return "Use Proxy";
    case 307:
      return "Temporary Redirect";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 402:
      return "Payment Required";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 407:
      return "Proxy Authentication Required";
    case 408:
      return "Request Time-out";
    case 409:
      return "Conflict";
    case 410:
      return "Gone";
    case 411:
      return "Length Required";
    case 412:
      return "Precondition Failed";
    case 413:
      return "Request Entity Too Large";
    case 414:
      return "Request-URI Too Large";
    case 415:
      return "Unsupported Media Type";
    case 416:
      return "Requested range not satisfiable";
    case 417:
      return "Expectation Failed";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Time-out";
    case 505:
      return "HTTP Version not supported";
    default:
      return "";
  }
}

AsyncWebServerResponse::AsyncWebServerResponse()
  : _code(0)
  , _headers(LinkedList<AsyncWebHeader*>([](AsyncWebHeader* h) { delete h; }))
  , _contentType()
  , _contentLength(0)
  , _sendContentLength(true)
  , _chunked(false)
  , _headLength(0)
  , _sentLength(0)
  , _ackedLength(0)
  , _writtenLength(0)
  , _state(RESPONSE_SETUP) {
  for (auto header : DefaultHeaders::Instance()) {
    _headers.add(new AsyncWebHeader(header->name(), header->value()));
  }
}

AsyncWebServerResponse::~AsyncWebServerResponse() {
  _headers.free();
}

void AsyncWebServerResponse::setCode(int code) {
  if (_state == RESPONSE_SETUP) _code = code;
}

void AsyncWebServerResponse::setContentLength(size_t len) {
  if (_state == RESPONSE_SETUP) _contentLength = len;
}

void AsyncWebServerResponse::setContentType(const String& type) {
  if (_state == RESPONSE_SETUP) _contentType = type;
}

void AsyncWebServerResponse::addHeader(const String& name, const String& value) {
  _headers.add(new AsyncWebHeader(name, value));
}

String AsyncWebServerResponse::_assembleHead(uint8_t version) {
  if (version) {
    addHeader("Accept-Ranges", "none");
    if (_chunked) addHeader("Transfer-Encoding", "chunked");
  }
  String out  = String();
  int bufSize = 300;
  char buf[bufSize];

  snprintf(buf, bufSize, "HTTP/1.%d %d %s\r\n", version, _code, _responseCodeToString(_code));
  out.concat(buf);

  if (_sendContentLength) {
    snprintf(buf, bufSize, "Content-Length: %d\r\n", _contentLength);
    out.concat(buf);
  }
  if (_contentType.length()) {
    snprintf(buf, bufSize, "Content-Type: %s\r\n", _contentType.c_str());
    out.concat(buf);
  }

  for (const auto& header : _headers) {
    snprintf(buf, bufSize, "%s: %s\r\n", header->name().c_str(), header->value().c_str());
    out.concat(buf);
  }
  _headers.free();

  out.concat("\r\n");
  _headLength = out.length();
  return out;
}

bool AsyncWebServerResponse::_started() const {
  return _state > RESPONSE_SETUP;
}
bool AsyncWebServerResponse::_finished() const {
  return _state > RESPONSE_WAIT_ACK;
}
bool AsyncWebServerResponse::_failed() const {
  return _state == RESPONSE_FAILED;
}
bool AsyncWebServerResponse::_sourceValid() const {
  return false;
}
void AsyncWebServerResponse::_respond(AsyncWebServerRequest* request) {
  _state = RESPONSE_END;
  request->client()->close();
}
size_t AsyncWebServerResponse::_ack(AsyncWebServerRequest* request, size_t len, uint32_t time) {
  (void)request;
  (void)len;
  (void)time;
  return 0;
}

/*
 * String/Code Response
 * */
AsyncBasicResponse::AsyncBasicResponse(int code, const String& contentType, const String& content) {
  _code        = code;
  _content     = content;
  _contentType = contentType;
  if (_content.length()) {
    _contentLength = _content.length();
    if (!_contentType.length()) _contentType = "text/plain";
  }
  addHeader("Connection", "close");
}

void AsyncBasicResponse::_respond(AsyncWebServerRequest* request) {
  _state        = RESPONSE_HEADERS;
  String out    = _assembleHead(request->version());
  size_t outLen = out.length();
  size_t space  = request->client()->space();
  if (!_contentLength && space >= outLen) {
    _writtenLength += request->client()->write(out.c_str(), outLen);
    _state = RESPONSE_WAIT_ACK;
  } else if (_contentLength && space >= outLen + _contentLength) {
    out += _content;
    outLen += _contentLength;
    _writtenLength += request->client()->write(out.c_str(), outLen);
    _state = RESPONSE_WAIT_ACK;
  } else if (space && space < outLen) {
    String partial = out.substring(0, space);
    _content       = out.substring(space) + _content;
    _contentLength += outLen - space;
    _writtenLength += request->client()->write(partial.c_str(), partial.length());
    _state = RESPONSE_CONTENT;
  } else if (space > outLen && space < (outLen + _contentLength)) {
    size_t shift = space - outLen;
    outLen += shift;
    _sentLength += shift;
    out += _content.substring(0, shift);
    _content = _content.substring(shift);
    _writtenLength += request->client()->write(out.c_str(), outLen);
    _state = RESPONSE_CONTENT;
  } else {
    _content = out + _content;
    _contentLength += outLen;
    _state = RESPONSE_CONTENT;
  }
}

size_t AsyncBasicResponse::_ack(AsyncWebServerRequest* request, size_t len, uint32_t time) {
  (void)time;
  _ackedLength += len;
  if (_state == RESPONSE_CONTENT) {
    size_t available = _contentLength - _sentLength;
    size_t space     = request->client()->space();
    // we can fit in this packet
    if (space > available) {
      _writtenLength += request->client()->write(_content.c_str(), available);
      _content = String();
      _state   = RESPONSE_WAIT_ACK;
      return available;
    }
    // send some data, the rest on ack
    String out = _content.substring(0, space);
    _content   = _content.substring(space);
    _sentLength += space;
    _writtenLength += request->client()->write(out.c_str(), space);
    return space;
  } else if (_state == RESPONSE_WAIT_ACK) {
    if (_ackedLength >= _writtenLength) {
      _state = RESPONSE_END;
    }
  }
  return 0;
}

/*
 * Abstract Response
 * */

AsyncAbstractResponse::AsyncAbstractResponse() {
}

void AsyncAbstractResponse::_respond(AsyncWebServerRequest* request) {
  addHeader("Connection", "close");
  _head  = _assembleHead(request->version());
  _state = RESPONSE_HEADERS;
  _ack(request, 0, 0);
}

size_t AsyncAbstractResponse::_ack(AsyncWebServerRequest* request, size_t len, uint32_t time) {
  (void)time;
  if (!_sourceValid()) {
    _state = RESPONSE_FAILED;
    request->client()->close();
    return 0;
  }
  _ackedLength += len;
  size_t space = request->client()->space();

  size_t headLen = _head.length();
  if (_state == RESPONSE_HEADERS) {
    if (space >= headLen) {
      _state = RESPONSE_CONTENT;
      space -= headLen;
    } else {
      String out = _head.substring(0, space);
      _head      = _head.substring(space);
      _writtenLength += request->client()->write(out.c_str(), out.length());
      return out.length();
    }
  }

  if (_state == RESPONSE_CONTENT) {
    size_t outLen;
    if (_chunked) {
      if (space <= 8) {
        return 0;
      }
      outLen = space;
    } else if (!_sendContentLength) {
      outLen = space;
    } else {
      outLen = ((_contentLength - _sentLength) > space) ? space : (_contentLength - _sentLength);
    }

    uint8_t* buf = (uint8_t*)malloc(outLen + headLen);
    if (!buf) {
      // os_printf("_ack malloc %d failed\n", outLen+headLen);
      return 0;
    }

    if (headLen) {
      memcpy(buf, _head.c_str(), _head.length());
    }

    size_t readLen = 0;

    if (_chunked) {
      // HTTP 1.1 allows leading zeros in chunk length.
      // See RFC2616 sections 2, 3.6.1.
      readLen = _fillBuffer(buf + headLen + 6, outLen - 8);
      if (readLen == RESPONSE_TRY_AGAIN) {
        free(buf);
        return 0;
      }
      outLen        = sprintf((char*)buf + headLen, "%04x", readLen) + headLen;
      buf[outLen++] = '\r';
      buf[outLen++] = '\n';
      outLen += readLen;
      buf[outLen++] = '\r';
      buf[outLen++] = '\n';
    } else {
      readLen = _fillBuffer(buf + headLen, outLen);
      if (readLen == RESPONSE_TRY_AGAIN) {
        free(buf);
        return 0;
      }
      outLen = readLen + headLen;
    }

    if (headLen) {
      _head = String();
    }

    if (outLen) {
      _writtenLength += request->client()->write((const char*)buf, outLen);
    }

    if (_chunked) {
      _sentLength += readLen;
    } else {
      _sentLength += outLen - headLen;
    }

    free(buf);

    if ((_chunked && readLen == 0) || (!_sendContentLength && outLen == 0) || (!_chunked && _sentLength == _contentLength)) {
      _state = RESPONSE_WAIT_ACK;
    }
    return outLen;
  } else if (_state == RESPONSE_WAIT_ACK) {
    if (!_sendContentLength || _ackedLength >= _writtenLength) {
      _state = RESPONSE_END;
      if (!_chunked && !_sendContentLength) request->client()->close(true);
    }
  }
  return 0;
}

size_t AsyncAbstractResponse::_readDataFromCacheOrContent(uint8_t* data, const size_t len) {
  // If we have something in cache, copy it to buffer
  const size_t readFromCache = std::min(len, _cache.size());
  if (readFromCache) {
    memcpy(data, _cache.data(), readFromCache);
    _cache.erase(_cache.begin(), _cache.begin() + readFromCache);
  }
  // If we need to read more...
  const size_t needFromFile    = len - readFromCache;
  const size_t readFromContent = _fillBuffer(data + readFromCache, needFromFile);
  return readFromCache + readFromContent;
}

/*
 * File Response
 * */

AsyncFileResponse::~AsyncFileResponse() {
  if (_content) _content.close();
}

void AsyncFileResponse::_setContentType(const String& path) {
  const char* const BINARY_MIME = "application/octet-stream";

  static std::unordered_map<String, const char* const> mimeTypes = {
    { ".html",              "text/html"},
    {  ".htm",              "text/html"},
    {  ".css",               "text/css"},
    { ".json",       "application/json"},
    {   ".js", "application/javascript"},
    {  ".png",              "image/png"},
    {  ".gif",              "image/gif"},
    {  ".jpg",             "image/jpeg"},
    {  ".ico",           "image/x-icon"},
    {  ".svg",          "image/svg+xml"},
    {  ".eot",               "font/eot"},
    { ".woff",              "font/woff"},
    {".woff2",             "font/woff2"},
    {  ".ttf",               "font/ttf"},
    {  ".xml",               "text/xml"},
    {  ".pdf",        "application/pdf"},
    {  ".zip",        "application/zip"},
    {   ".gz",     "application/x-gzip"},
    {  ".txt",             "text/plain"},
    {  ".bin",                 BINARY_MIME},
  };

  int lastDot = path.lastIndexOf('.');
  if (lastDot < 0) {
    _contentType = BINARY_MIME;
    return;
  }

  String extension = path.substring(lastDot);

  auto it = mimeTypes.find(extension);
  if (it == mimeTypes.end()) {
    _contentType = BINARY_MIME;
    return;
  }

  _contentType = it->second;
}

AsyncFileResponse::AsyncFileResponse(FS& fs, const String& path, const String& contentType, bool download) : AsyncAbstractResponse() {
  _code = 200;
  _path = path;

  if (!download && !fs.exists(_path) && fs.exists(_path + ".gz")) {
    _path = _path + ".gz";
    addHeader("Content-Encoding", "gzip");
    _sendContentLength = true;
    _chunked           = false;
  }

  _content       = fs.open(_path, "rb");
  _contentLength = _content.size();

  if (contentType == "")
    _setContentType(path);
  else
    _contentType = contentType;

  int filenameStart = path.lastIndexOf('/') + 1;
  char buf[26 + path.length() - filenameStart];
  char* filename = (char*)path.c_str() + filenameStart;

  if (download) {
    // set filename and force download
    snprintf(buf, sizeof(buf), "attachment; filename=\"%s\"", filename);
  } else {
    // set filename and force rendering
    snprintf(buf, sizeof(buf), "inline; filename=\"%s\"", filename);
  }
  addHeader("Content-Disposition", buf);
}

AsyncFileResponse::AsyncFileResponse(File content, const String& path, const String& contentType, bool download) : AsyncAbstractResponse() {
  _code = 200;
  _path = path;

  if (!download && String(content.name()).endsWith(".gz") && !path.endsWith(".gz")) {
    addHeader("Content-Encoding", "gzip");
    _sendContentLength = true;
    _chunked           = false;
  }

  _content       = content;
  _contentLength = _content.size();

  if (contentType == "")
    _setContentType(path);
  else
    _contentType = contentType;

  int filenameStart = path.lastIndexOf('/') + 1;
  char buf[26 + path.length() - filenameStart];
  char* filename = (char*)path.c_str() + filenameStart;

  if (download) {
    snprintf(buf, sizeof(buf), "attachment; filename=\"%s\"", filename);
  } else {
    snprintf(buf, sizeof(buf), "inline; filename=\"%s\"", filename);
  }
  addHeader("Content-Disposition", buf);
}

size_t AsyncFileResponse::_fillBuffer(uint8_t* data, size_t len) {
  return _content.read(data, len);
}

/*
 * Stream Response
 * */

AsyncStreamResponse::AsyncStreamResponse(Stream& stream, const String& contentType, size_t len) : AsyncAbstractResponse() {
  _code          = 200;
  _content       = &stream;
  _contentLength = len;
  _contentType   = contentType;
}

size_t AsyncStreamResponse::_fillBuffer(uint8_t* data, size_t len) {
  size_t available = _content->available();
  size_t outLen    = (available > len) ? len : available;
  size_t i;
  for (i = 0; i < outLen; i++) data[i] = _content->read();
  return outLen;
}

/*
 * Callback Response
 * */

AsyncCallbackResponse::AsyncCallbackResponse(const String& contentType, size_t len, AwsResponseFiller callback) : AsyncAbstractResponse() {
  _code          = 200;
  _content       = callback;
  _contentLength = len;
  if (!len) _sendContentLength = false;
  _contentType  = contentType;
  _filledLength = 0;
}

size_t AsyncCallbackResponse::_fillBuffer(uint8_t* data, size_t len) {
  size_t ret = _content(data, len, _filledLength);
  if (ret != RESPONSE_TRY_AGAIN) {
    _filledLength += ret;
  }
  return ret;
}

/*
 * Chunked Response
 * */

AsyncChunkedResponse::AsyncChunkedResponse(const String& contentType, AwsResponseFiller callback) : AsyncAbstractResponse() {
  _code              = 200;
  _content           = callback;
  _contentLength     = 0;
  _contentType       = contentType;
  _sendContentLength = false;
  _chunked           = true;
  _filledLength      = 0;
}

size_t AsyncChunkedResponse::_fillBuffer(uint8_t* data, size_t len) {
  size_t ret = _content(data, len, _filledLength);
  if (ret != RESPONSE_TRY_AGAIN) {
    _filledLength += ret;
  }
  return ret;
}

/*
 * Progmem Response
 * */

AsyncProgmemResponse::AsyncProgmemResponse(int code, const String& contentType, const uint8_t* content, size_t len) : AsyncAbstractResponse() {
  _code          = code;
  _content       = content;
  _contentType   = contentType;
  _contentLength = len;
  _readLength    = 0;
}

size_t AsyncProgmemResponse::_fillBuffer(uint8_t* data, size_t len) {
  size_t left = _contentLength - _readLength;
  if (left > len) {
    memcpy_P(data, _content + _readLength, len);
    _readLength += len;
    return len;
  }
  memcpy_P(data, _content + _readLength, left);
  _readLength += left;
  return left;
}

/*
 * Response Stream (You can print/write/printf to it, up to the contentLen bytes)
 * */

AsyncResponseStream::AsyncResponseStream(const String& contentType, size_t bufferSize) {
  _code          = 200;
  _contentLength = 0;
  _contentType   = contentType;
  _content       = new cbuf(bufferSize);
}

AsyncResponseStream::~AsyncResponseStream() {
  delete _content;
}

size_t AsyncResponseStream::_fillBuffer(uint8_t* buf, size_t maxLen) {
  return _content->read((char*)buf, maxLen);
}

size_t AsyncResponseStream::write(const uint8_t* data, size_t len) {
  if (_started()) return 0;

  if (len > _content->room()) {
    size_t needed = len - _content->room();
    _content->resizeAdd(needed);
  }
  size_t written = _content->write((const char*)data, len);
  _contentLength += written;
  return written;
}

size_t AsyncResponseStream::write(uint8_t data) {
  return write(&data, 1);
}
