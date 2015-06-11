/* Make sure select() works with >1024 file handles open. */
#include <sys/types.h>
#undef __FD_SETSIZE
#define __FD_SETSIZE 524288 

#include <typhoeus_multi.h>
#include <errno.h>

static void multi_read_info(VALUE self, CURLM *multi_handle);

static void dealloc(CurlMulti *curl_multi) {
  curl_multi_cleanup(curl_multi->multi);
  free(curl_multi);
}

static VALUE multi_add_handle(VALUE self, VALUE easy) {
  CurlEasy *curl_easy;
  CurlMulti *curl_multi;
  CURLMcode mcode;
  VALUE easy_handles;

  Data_Get_Struct(easy, CurlEasy, curl_easy);
  Data_Get_Struct(self, CurlMulti, curl_multi);

  mcode = curl_multi_add_handle(curl_multi->multi, curl_easy->curl);
  if (mcode != CURLM_CALL_MULTI_PERFORM && mcode != CURLM_OK) {
    rb_raise(rb_eRuntimeError, "An error occured adding the handle: %d: %s", mcode, curl_multi_strerror(mcode));
  }

  curl_easy_setopt(curl_easy->curl, CURLOPT_PRIVATE, easy);
  curl_multi->active++;

  easy_handles = rb_iv_get(self, "@easy_handles");
  rb_ary_push(easy_handles, easy);

  if (mcode == CURLM_CALL_MULTI_PERFORM) {
    curl_multi_perform(curl_multi->multi, &(curl_multi->running));
  }
  // 
  // if (curl_multi->running) {
  //     printf("call read_info on add<br/>");
  //   multi_read_info(self, curl_multi->multi);
  // }

  return easy;
}

static VALUE multi_remove_handle(VALUE self, VALUE easy) {
  CurlEasy *curl_easy;
  CurlMulti *curl_multi;
  VALUE easy_handles;

  Data_Get_Struct(easy, CurlEasy, curl_easy);
  Data_Get_Struct(self, CurlMulti, curl_multi);

  curl_multi->active--;
  curl_multi_remove_handle(curl_multi->multi, curl_easy->curl);

  easy_handles = rb_iv_get(self, "@easy_handles");
  rb_ary_delete(easy_handles, easy);

  return easy;
}

static void multi_read_info(VALUE self, CURLM *multi_handle) {
  int msgs_left, result;
  CURLMsg *msg;
  CURLcode ecode;
  CURL *easy_handle;
  VALUE easy;
  long response_code;

  /* check for finished easy handles and remove from the multi handle */
  while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {

    if (msg->msg != CURLMSG_DONE) {
      continue;
    }

    easy_handle = msg->easy_handle;
    result = msg->data.result;
    if (easy_handle) {
      ecode = curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &easy);
      if (ecode != 0) {
        rb_raise(rb_eRuntimeError, "error getting easy object: %d: %s", ecode, curl_easy_strerror(ecode));
      }

      response_code = -1;
      curl_easy_getinfo(easy_handle, CURLINFO_RESPONSE_CODE, &response_code);

      multi_remove_handle(self, easy);
      rb_iv_set(easy, "@curl_return_code", INT2FIX(result));

      if (result != 0) {
        rb_funcall(easy, rb_intern("failure"), 0);
      }
      else if ((response_code >= 200 && response_code < 300) || response_code == 0) {
        rb_funcall(easy, rb_intern("success"), 0);
      }
      else if (response_code >= 300 && response_code < 600) {
        rb_funcall(easy, rb_intern("failure"), 0);
      }
    }
  }
}

/* called by multi_perform and fire_and_forget */
static void rb_curl_multi_run(VALUE self, CURLM *multi_handle, int *still_running) {
  CURLMcode mcode;

  do {
    mcode = curl_multi_perform(multi_handle, still_running);
  } while (mcode == CURLM_CALL_MULTI_PERFORM);

  if (mcode != CURLM_OK) {
    rb_raise(rb_eRuntimeError, "an error occured while running perform: %d: %s", mcode, curl_multi_strerror(mcode));
  }

  multi_read_info( self, multi_handle );
}

static VALUE fire_and_forget(VALUE self) {
  CurlMulti *curl_multi;
  Data_Get_Struct(self, CurlMulti, curl_multi);
  rb_curl_multi_run( self, curl_multi->multi, &(curl_multi->running) );

  return Qnil;
}

#define FD_SET_INIT do { \
  rb_fd_init(&fdread);   \
  rb_fd_init(&fdwrite);  \
  rb_fd_init(&fdexcep);  \
  } while (0);

#define FD_SET_ZERO do { \
  rb_fd_zero(&fdread);   \
  rb_fd_zero(&fdwrite);  \
  rb_fd_zero(&fdexcep);  \
  } while (0);

#define FD_SET_TERM do { \
  rb_fd_term(&fdread);   \
  rb_fd_term(&fdwrite);  \
  rb_fd_term(&fdexcep);  \
  } while (0);

static VALUE multi_perform(VALUE self) {
  CURLMcode mcode;
  CurlMulti *curl_multi;
  int maxfd, rc;
  rb_fdset_t fdread, fdwrite, fdexcep;
  FD_SET_INIT;

  long timeout;
  struct timeval tv = {0, 0};

  Data_Get_Struct(self, CurlMulti, curl_multi);

  rb_curl_multi_run( self, curl_multi->multi, &(curl_multi->running) );
  while(curl_multi->running) {
    FD_SET_ZERO;

    /* get the curl suggested time out */
    mcode = curl_multi_timeout(curl_multi->multi, &timeout);
    if (mcode != CURLM_OK) {
      FD_SET_TERM;
      rb_raise(rb_eRuntimeError, "an error occured getting the timeout: %d: %s", mcode, curl_multi_strerror(mcode));
    }

    if (timeout == 0) { /* no delay */
      rb_curl_multi_run( self, curl_multi->multi, &(curl_multi->running) );
      continue;
    }
    else if (timeout < 0) {
      timeout = 1;
    }

    tv.tv_sec = timeout / 1000;
    tv.tv_usec = ((int)timeout * 1000) % 1000000;

    /* load the fd sets from the multi handle */
    mcode = curl_multi_fdset(curl_multi->multi, fdread.fdset, fdwrite.fdset, fdexcep.fdset, &maxfd);
    if (mcode != CURLM_OK) {
      FD_SET_TERM;
      rb_raise(rb_eRuntimeError, "an error occured getting the fdset: %d: %s", mcode, curl_multi_strerror(mcode));
    }

    rc = rb_thread_fd_select(maxfd+1, &fdread, &fdwrite, &fdexcep, &tv);
    if (rc < 0) {
      FD_SET_TERM;
      rb_raise(rb_eRuntimeError, "error on thread select: %d", errno);
    }
    rb_curl_multi_run( self, curl_multi->multi, &(curl_multi->running) );

  }

  FD_SET_TERM;

  return Qnil;
}

static VALUE active_handle_count(VALUE self) {
  CurlMulti *curl_multi;
  Data_Get_Struct(self, CurlMulti, curl_multi);

  return INT2FIX(curl_multi->active);
}

static VALUE multi_cleanup(VALUE self) {
  CurlMulti *curl_multi;
  Data_Get_Struct(self, CurlMulti, curl_multi);

  curl_multi_cleanup(curl_multi->multi);
  curl_multi->active = 0;
  curl_multi->running = 0;

  return Qnil;
}

#undef FD_SET_INIT
#undef FD_SET_ZERO
#undef FD_SET_TERM

static VALUE new(int argc, VALUE *argv, VALUE klass ARG_UNUSED) {
  CurlMulti *curl_multi = ALLOC(CurlMulti);
  VALUE multi;
  curl_multi->multi = curl_multi_init();
  curl_multi->active = 0;
  curl_multi->running = 0;

  multi = Data_Wrap_Struct(cTyphoeusMulti, 0, dealloc, curl_multi);

  rb_obj_call_init(multi, argc, argv);

  return multi;
}

void init_typhoeus_multi() {
  VALUE klass = cTyphoeusMulti = rb_define_class_under(mTyphoeus, "Multi", rb_cObject);

  rb_define_singleton_method(klass, "new", new, -1);
  rb_define_private_method(klass, "multi_add_handle", multi_add_handle, 1);
  rb_define_private_method(klass, "multi_remove_handle", multi_remove_handle, 1);
  rb_define_private_method(klass, "multi_perform", multi_perform, 0);
  rb_define_private_method(klass, "multi_cleanup", multi_cleanup, 0);
  rb_define_private_method(klass, "active_handle_count", active_handle_count, 0);
	rb_define_method(klass, "fire_and_forget", fire_and_forget, 0);
}
