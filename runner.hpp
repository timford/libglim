#ifndef _GLIM_RUNNER_INCLUDED
#define _GLIM_RUNNER_INCLUDED

#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <stdexcept>
#include <memory>

#include <curl/curl.h>
#include <event2/event.h>

#include "gstring.hpp"

namespace glim {

/** Run CURLM requests and completion handlers, as well as other periodic jobs. */
class Runner {
  /** Free CURL during stack unwinding. */
  struct FreeCurl {
    Runner* runner; CURL* curl;
    FreeCurl (Runner* runner, CURL* curl): runner (runner), curl (curl) {}
    ~FreeCurl() {
      runner->_handlers.erase (curl);
      curl_multi_remove_handle (runner->_curlm, curl);
      curl_easy_cleanup (curl);
    }
  };
protected:
  typedef std::function<void(CURLMsg*)> handler_t;
  typedef std::function<void()> job_t;
  typedef std::function<void(const char* error)> errlog_t;
  std::shared_ptr<struct event_base> _evbase;
  errlog_t _errlog;
  std::mutex _mutex;
  std::map<CURL*, handler_t> _handlers;
  /** Functions to run periodically. */
  std::map<glim::gstring, job_t> _jobs;
  CURLM* _curlm = NULL;
  struct event* _timer = NULL;

  /** Should only be run when the _mutex is locked. */
  void runCurlUnderLock () {
    int running_handles = 0;
    CURLMcode ret = curl_multi_perform (_curlm, &running_handles);
    if (ret != CURLM_OK) {
      char eBuf[256]; glim::gstring err (sizeof(eBuf), eBuf, false, 0);
      err << "glim::Runner: curl_multi_perform: " << curl_multi_strerror (ret);
      _errlog (err.c_str());
    }
    nextMessage:
      int msgs_in_queue = 0;
      CURLMsg* msg = curl_multi_info_read (_curlm, &msgs_in_queue);
      if (msg) try {
        auto curl = msg->easy_handle;
        FreeCurl freeCurl (this, curl);
        auto it = _handlers.find (curl);
        if (it != _handlers.end()) it->second (msg);
        if (msgs_in_queue > 0) goto nextMessage;
      } catch (const std::exception& ex) {
        char eBuf[512]; glim::gstring err (sizeof(eBuf), eBuf, false, 0);
        err << "glim::Runner: handler: " << ex.what();
        _errlog (err.c_str());
      }
    restartTimer();
  }
  void restartTimer() {
    if (_timer) {
      struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000; // tv_usec is in microseconds
      evtimer_add (_timer, &tv);
    }
  }
  static void timerCB (evutil_socket_t, short, void* rpt) {
    ((Runner*) rpt)->run();
  };
public:
  Runner (std::shared_ptr<struct event_base> evbase, errlog_t errlog): _evbase (evbase), _errlog (errlog) {
    _curlm = curl_multi_init();
    _timer = evtimer_new (evbase.get(), timerCB, this);
    restartTimer();
  }
  ~Runner() {
    std::unique_lock<std::mutex> lock (_mutex);
    if (_timer) {evtimer_del (_timer); _timer = NULL;}
    for (auto it = _handlers.begin(), end = _handlers.end(); it != end; ++it) {
      curl_multi_remove_handle (_curlm, it->first);
      curl_easy_cleanup (it->first);
    }
    if (_curlm) {curl_multi_cleanup (_curlm); _curlm = NULL;}
  }
  /** Wait for the operation to complete, then call the `handler`, then free the `curl`. */
  void multi (CURL* curl, handler_t handler) {
    std::unique_lock<std::mutex> lock (_mutex);
    curl_multi_add_handle (_curlm, curl);
    _handlers[curl] = handler;
    runCurlUnderLock();
  }
  /** Register a new job to be run on the thread loop. */
  std::function<void()>& job (const glim::gstring& name) {
    std::unique_lock<std::mutex> lock (_mutex);
    return _jobs[name];
  }
  void removeJob (const glim::gstring& name) {
    std::unique_lock<std::mutex> lock (_mutex);
    _jobs.erase (name);
  }
  /** Invoked automatically from a libevent timer; can also be invoked manually. */
  void run() {
    std::unique_lock<std::mutex> lock (_mutex);
    runCurlUnderLock();
    // Run non-CURL jobs.
    for (auto it = _jobs.begin(), end = _jobs.end(); it != end; ++it) try {
      it->second ();
    } catch (const std::exception& ex) {
      char eBuf[512]; glim::gstring err (sizeof(eBuf), eBuf, false, 0);
      err << "glim::Runner: error in job " << it->first << ": " << ex.what();
      _errlog (err.c_str());
    }
  }
};

} // namespace glim

#endif // _GLIM_RUNNER_INCLUDED
