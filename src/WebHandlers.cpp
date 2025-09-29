// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright 2016-2025 Hristo Gochkov, Mathieu Carbou, Emil Muratov

#include "ESPAsyncWebServer.h"
#include "WebHandlerImpl.h"
#include "AsyncWebServerLogging.h"

using namespace asyncsrv;

AsyncWebHandler &AsyncWebHandler::setFilter(ArRequestFilterFunction fn) {
  _filter = fn;
  return *this;
}
AsyncWebHandler &AsyncWebHandler::setAuthentication(const char *username, const char *password, AsyncAuthType authMethod) {
  if (!_authMiddleware) {
    _authMiddleware = new AsyncAuthenticationMiddleware();
    _authMiddleware->_freeOnRemoval = true;
    addMiddleware(_authMiddleware);
  }
  _authMiddleware->setUsername(username);
  _authMiddleware->setPassword(password);
  _authMiddleware->setAuthType(authMethod);
  return *this;
};

AsyncStaticWebHandler::AsyncStaticWebHandler(const char *uri, FS &fs, const char *path, const char *cache_control)
  : _fs(fs), _uri(uri), _path(path), _default_file(F("index.htm")), _cache_control(cache_control), _shared_eTag(""), _callback(nullptr) {
  // Ensure leading '/'
  if (_uri.length() == 0 || _uri[0] != '/') {
    _uri = String('/') + _uri;
  }
  if (_path.length() == 0 || _path[0] != '/') {
    _path = String('/') + _path;
  }

  // If path ends with '/' we assume a hint that this is a directory to improve performance.
  // However - if it does not end with '/' we, can't assume a file, path can still be a directory.
  _isDir = _path[_path.length() - 1] == '/';

  // Remove the trailing '/' so we can handle default file
  // Notice that root will be "" not "/"
  if (_uri[_uri.length() - 1] == '/') {
    _uri = _uri.substring(0, _uri.length() - 1);
  }
  if (_path[_path.length() - 1] == '/') {
    _path = _path.substring(0, _path.length() - 1);
  }
}

AsyncStaticWebHandler &AsyncStaticWebHandler::setTryGzipFirst(bool value) {
  _tryGzipFirst = value;
  return *this;
}

AsyncStaticWebHandler &AsyncStaticWebHandler::setIsDir(bool isDir) {
  _isDir = isDir;
  return *this;
}

AsyncStaticWebHandler &AsyncStaticWebHandler::setDefaultFile(const char *filename) {
  _default_file = filename;
  return *this;
}

AsyncStaticWebHandler &AsyncStaticWebHandler::setCacheControl(const char *cache_control) {
  _cache_control = cache_control;
  return *this;
}

AsyncStaticWebHandler &AsyncStaticWebHandler::setSharedEtag(const char *etag) {
  _shared_eTag = etag;
  return *this;
}

bool AsyncStaticWebHandler::canHandle(AsyncWebServerRequest *request) const {
  return request->isHTTP() && request->method() == HTTP_GET && request->url().startsWith(_uri) && _getFile(request);
}

bool AsyncStaticWebHandler::_getFile(AsyncWebServerRequest *request) const {
  // Remove the found uri
  String path = request->url().substring(_uri.length());

  // We can skip the file check and look for default if request is to the root of a directory or that request path ends with '/'
  bool canSkipFileCheck = (_isDir && path.length() == 0) || (path.length() && path[path.length() - 1] == '/');

  path = _path + path;

  // Do we have a file or .gz file
  if (!canSkipFileCheck && const_cast<AsyncStaticWebHandler *>(this)->_searchFile(request, path)) {
    return true;
  }

  // Can't handle if not default file
  if (_default_file.length() == 0) {
    return false;
  }

  // Try to add default file, ensure there is a trailing '/' to the path.
  if (path.length() == 0 || path[path.length() - 1] != '/') {
    path += String('/');
  }
  path += _default_file;

  return const_cast<AsyncStaticWebHandler *>(this)->_searchFile(request, path);
}

#ifdef ESP32
#define FILE_IS_REAL(f) (f == true && !f.isDirectory())
#else
#define FILE_IS_REAL(f) (f == true)
#endif

bool AsyncStaticWebHandler::_searchFile(AsyncWebServerRequest *request, const String &path) {
  bool fileFound = false;
  bool gzipFound = false;

  String gzip = path + T__gz;

  if (_tryGzipFirst) {
    if (_fs.exists(gzip)) {
      request->_tempFile = _fs.open(gzip, fs::FileOpenMode::read);
      gzipFound = FILE_IS_REAL(request->_tempFile);
    }
    if (!gzipFound) {
      if (_fs.exists(path)) {
        request->_tempFile = _fs.open(path, fs::FileOpenMode::read);
        fileFound = FILE_IS_REAL(request->_tempFile);
      }
    }
  } else {
    if (_fs.exists(path)) {
      request->_tempFile = _fs.open(path, fs::FileOpenMode::read);
      fileFound = FILE_IS_REAL(request->_tempFile);
    }
    if (!fileFound) {
      if (_fs.exists(gzip)) {
        request->_tempFile = _fs.open(gzip, fs::FileOpenMode::read);
        gzipFound = FILE_IS_REAL(request->_tempFile);
      }
    }
  }

  bool found = fileFound || gzipFound;

  if (found) {
    // Extract the file name from the path and keep it in _tempObject
    size_t pathLen = path.length();
    char *_tempPath = (char *)malloc(pathLen + 1);
    if (_tempPath == NULL) {
      async_ws_log_e("Failed to allocate");
      request->abort();
      request->_tempFile.close();
      return false;
    }
    snprintf_P(_tempPath, pathLen + 1, PSTR("%s"), path.c_str());
    request->_tempObject = (void *)_tempPath;
  }

  return found;
}

void AsyncStaticWebHandler::handleRequest(AsyncWebServerRequest *request) {
  // Get the filename from request->_tempObject and free it
  String filename((char *)request->_tempObject);
  free(request->_tempObject);
  request->_tempObject = NULL;

  if (request->_tempFile != true) {
    request->send(404);
    return;
  }

  bool not_modified = false;

  if (_shared_eTag.length()) {
    not_modified = request->header(T_IMS).equals(_shared_eTag);
  }

  AsyncWebServerResponse *response;

  if (not_modified) {
    request->_tempFile.close();
    response = new AsyncBasicResponse(304);  // Not modified
  } else {
    response = new AsyncFileResponse(request->_tempFile, filename, emptyString, false, _callback);
  }

  if (!response) {
    async_ws_log_e("Failed to allocate");
    request->abort();
    return;
  }

  if (_shared_eTag.length()) {
    response->addHeader(T_ETag, _shared_eTag.c_str());
  }
  if (_cache_control.length()) {
    response->addHeader(T_Cache_Control, _cache_control.c_str());
  }

  request->send(response);
}

AsyncStaticWebHandler &AsyncStaticWebHandler::setTemplateProcessor(AwsTemplateProcessor newCallback) {
  _callback = newCallback;
  return *this;
}

void AsyncCallbackWebHandler::setUri(const String &uri) {
  _uri = uri;
  _isRegex = uri.startsWith("^") && uri.endsWith("$");
}

bool AsyncCallbackWebHandler::canHandle(AsyncWebServerRequest *request) const {
  if (!_onRequest || !request->isHTTP() || !(_method & request->method())) {
    return false;
  }

#ifdef ASYNCWEBSERVER_REGEX
  if (_isRegex) {
    std::regex pattern(_uri.c_str());
    std::smatch matches;
    std::string s(request->url().c_str());
    if (std::regex_search(s, matches, pattern)) {
      for (size_t i = 1; i < matches.size(); ++i) {  // start from 1
        request->_addPathParam(matches[i].str().c_str());
      }
    } else {
      return false;
    }
  } else
#endif
    if (_uri.length() && _uri.startsWith("/*.")) {
    String uriTemplate = String(_uri);
    uriTemplate = uriTemplate.substring(uriTemplate.lastIndexOf("."));
    if (!request->url().endsWith(uriTemplate)) {
      return false;
    }
  } else if (_uri.length() && _uri.endsWith("*")) {
    String uriTemplate = String(_uri);
    uriTemplate = uriTemplate.substring(0, uriTemplate.length() - 1);
    if (!request->url().startsWith(uriTemplate)) {
      return false;
    }
  } else if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) {
    return false;
  }

  return true;
}

void AsyncCallbackWebHandler::handleRequest(AsyncWebServerRequest *request) {
  if (_onRequest) {
    _onRequest(request);
  } else {
    request->send(404, T_text_plain, "Not found");
  }
}
void AsyncCallbackWebHandler::handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (_onUpload) {
    _onUpload(request, filename, index, data, len, final);
  }
}
void AsyncCallbackWebHandler::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  // ESP_LOGD("AsyncWebServer", "AsyncCallbackWebHandler::handleBody");
  if (_onBody) {
    _onBody(request, data, len, index, total);
  }
}
