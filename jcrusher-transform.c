/** @file

  This plugin crush JSON bodies, removing pretty formatting. This allows
  bandwith as well as cache space saving.

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

/* jcrusher-transform.c:  a simple transform plugin which read application/json
 *                        response body and re-dump it removing useless spaces
 *                        and new lines.
 *
 *    Usage:
 *      jcrusher.so
 * 
 */

/* set tab stops to four. */

#include <stdio.h>
#include <string.h>

#include "ts/ts.h"
#include "ink_defs.h"
#include <json-c/json.h>

#define TS_NULL_MUTEX      NULL
#define STATE_BUFFER_DATA   0
#define STATE_OUTPUT_DATA   1

typedef struct
{
  int state;

  TSHttpTxn txn;

  TSVIO downstream_vio;
  TSIOBuffer downstream_buffer;
  TSIOBufferReader downstream_reader;

  enum json_tokener_error json_err;
  json_object *json_obj;
  json_tokener *json_tok;

} JCrusherData;

static JCrusherData *
jcrusher_data_alloc()
{
  JCrusherData *data;

  data = (JCrusherData *) TSmalloc(sizeof(JCrusherData));
  data->state = STATE_BUFFER_DATA;

  data->downstream_vio = NULL;
  data->downstream_buffer = NULL;
  data->downstream_reader = NULL;

  data->json_tok = json_tokener_new();
  data->json_obj = NULL;
  return data;
}

static void
jcrusher_data_destroy(JCrusherData * data)
{
  TSDebug("jcrusher", "Start of jcrusher_data_destroy()");
  if (data) {
    if (data->downstream_buffer) {
      TSDebug("jcrusher", "jcrusher_data_destroy - destroying downstream buffer");
      TSIOBufferDestroy(data->downstream_buffer);
    }
    if (data->json_obj) {
      TSDebug("jcrusher", "jcrusher_data_destroy - destroying json object");
      json_object_put(data->json_obj);
      data->json_obj = NULL;
      TSDebug("jcrusher", "jcrusher_data_destroy - destroying json object -> done");
    }
    if (data->json_tok) {
      TSDebug("jcrusher", "jcrusher_data_destroy - destroying json tokener");
      json_tokener_free(data->json_tok);
      data->json_tok = NULL;
      TSDebug("jcrusher", "jcrusher_data_destroy - destroying json tokener -> done");
    }
    TSDebug("jcrusher", "jcrusher_data_destroy - Freeing data");
    TSfree(data);
    TSDebug("jcrusher", "jcrusher_data_destroy - Freeing data -> done");
  }
  TSDebug("jcrusher", "End of jcrusher_data_destroy()");
}

static int
handle_buffering(TSCont contp, JCrusherData * data)
{
  TSVIO upstream_vio;
  TSIOBuffer upstream_buffer;
  int64_t toread;
  int64_t avail;

  TSDebug("jcrusher", "Start of handle_buffering()");
  /* Get the write VIO for the write operation that was performed on
     ourself. This VIO contains the buffer that we are to read from
     as well as the continuation we are to call when the buffer is
     empty. */
  upstream_vio = TSVConnWriteVIOGet(contp);

  /* Create the output buffer and its associated reader */
  if (!data->downstream_buffer) {
    data->downstream_buffer = TSIOBufferCreate();
    TSAssert(data->downstream_buffer);
    data->downstream_reader = TSIOBufferReaderAlloc(data->downstream_buffer);
    TSAssert(data->downstream_reader);
  }

  /* We also check to see if the write VIO's buffer is non-NULL. A
     NULL buffer indicates that the write operation has been
     shutdown and that the continuation does not want us to send any
     more WRITE_READY or WRITE_COMPLETE events. For this buffered
     transformation that means we're done buffering data. */

  upstream_buffer = TSVIOBufferGet(upstream_vio);
  if (NULL == upstream_buffer) {
    data->state = STATE_OUTPUT_DATA;
    TSDebug("jcrusher", "handle_buffering - upstream_buffer is NULL");
    return 0;
  }

  /* Determine how much data we have left to read. For this bnull
     transform plugin this is also the amount of data we have left
     to write to the output connection. */

  toread = TSVIONTodoGet(upstream_vio);
  TSDebug("jcrusher", "handle_buffering - toread is %" PRId64, toread);
  if (toread > 0) {
    /* The amount of data left to read needs to be truncated by
       the amount of data actually in the read buffer. */

    avail = TSIOBufferReaderAvail(TSVIOReaderGet(upstream_vio));
    if (toread > avail) {
      toread = avail;
    }
    TSDebug("jcrusher", "handle_buffering - toread is %" PRId64, toread);
    TSDebug("jcrusher", "handle_buffering - avail is %" PRId64, avail);

    TSIOBufferReader upstream_reader = TSVIOReaderGet(upstream_vio);
    TSIOBufferBlock upstream_blk = TSIOBufferReaderStart(upstream_reader);
    const char *input = TSIOBufferBlockReadStart(upstream_blk, upstream_reader, &toread);
    TSDebug("jcrusher", "handle_buffering - just read [%d] bytes from buffer", (int)strlen(input));

    TSDebug("jcrusher", "handle_buffering - parse json input");
    data->json_obj = json_tokener_parse_ex(data->json_tok, input, strlen(input));
    if (json_tokener_success == (data->json_err = json_tokener_get_error(data->json_tok))) {
      TSDebug("jcrusher", "handle_buffering - got json_tokener_success");
      data->state = STATE_OUTPUT_DATA;
      /* Call back the write VIO continuation to let it know that we
         have completed the write operation. */
      TSContCall(TSVIOContGet(upstream_vio), TS_EVENT_VCONN_WRITE_COMPLETE, upstream_vio);
      return 1;
    }
    TSDebug("jcrusher", "handle_buffering - got json_tokener_continue");

    /* Tell the read buffer that we have read the data and are no
       longer interested in it. */
    TSIOBufferReaderConsume(TSVIOReaderGet(upstream_vio), toread);

    /* Modify the upstream VIO to reflect how much data we've
       completed. */
    TSVIONDoneSet(upstream_vio, TSVIONDoneGet(upstream_vio) + toread);

    /* Now we check the upstream VIO to see if there is data left to read. */
    /* Call back the upstream VIO continuation to let it know that we
       are ready for more data. */
    TSContCall(TSVIOContGet(upstream_vio), TS_EVENT_VCONN_WRITE_READY, upstream_vio);
  } else {
    TSDebug("jcrusher", "handle_buffering - seems we read all");
    data->state = STATE_OUTPUT_DATA;
    /* Call back the write VIO continuation to let it know that we
       have completed the write operation. */
    TSContCall(TSVIOContGet(upstream_vio), TS_EVENT_VCONN_WRITE_COMPLETE, upstream_vio);
  }

  TSDebug("jcrusher", "handle_buffering - End");
  return 1;
}

static int
handle_output(TSCont contp, JCrusherData * data)
{
  const char *output;
  int64_t written_bytes;

  /* Check to see if we need to initiate the output operation. */
  TSDebug("jcrusher", "Start of handle_output()");
  if (!data->downstream_vio) {
    TSVConn output_conn;

    /* Get the json_object as string and write it into buffer */
    output = json_object_to_json_string_ext(data->json_obj, JSON_C_TO_STRING_PLAIN);
    written_bytes = TSIOBufferWrite(data->downstream_buffer, output, (int64_t)(strlen(output)));
    TSDebug("jcrusher", "handle_output - Just write %" PRId64 " bytes to ouput", written_bytes);
    /* Get the output connection where we'll write data to. */
    output_conn = TSTransformOutputVConnGet(contp);
    data->downstream_vio =
      TSVConnWrite(output_conn, contp, data->downstream_reader, TSIOBufferReaderAvail(data->downstream_reader));

    TSAssert(data->downstream_vio);
  }
  TSDebug("jcrusher", "End of handle_output()");
  return 1;
}

static void
jcrusher_handle_transform(TSCont contp)
{
  JCrusherData *data;
  int done;

  /* Get our data structure for this operation. The private data
     structure contains the output VIO and output buffer. If the
     private data structure pointer is NULL, then we'll create it
     and initialize its internals. */

  TSDebug("jcrusher", "Start of handle_transform()");
  data = TSContDataGet(contp);
  if (!data) {
    data = jcrusher_data_alloc();
    TSContDataSet(contp, data);
  }

  do {
    switch (data->state) {
    case STATE_BUFFER_DATA:
      TSDebug("jcrusher", "data->state is STATE_BUFFER_DATA");
      done = handle_buffering(contp, data);
      break;
    case STATE_OUTPUT_DATA:
      TSDebug("jcrusher", "data->state is STATE_OUTPUT_DATA");
      done = handle_output(contp, data);
      break;
    default:
      TSDebug("jcrusher", "data->state is UNKNOWN");
      done = 1;
      break;
    }
  } while (!done);

  TSDebug("jcrusher", "End of handle_transform()");
}

static int
jcrusher_transform(TSCont contp, TSEvent event, void *edata ATS_UNUSED)
{
  /* Check to see if the transformation has been closed by a
     call to TSVConnClose. */
  if (TSVConnClosedGet(contp)) {
    TSDebug("jcrusher", "jcrusher_transform - transformation is closed. We're done\n");
    jcrusher_data_destroy(TSContDataGet(contp));
    TSContDestroy(contp);
  } else {
    TSDebug("jcrusher", "jcrusher_transform - transformation is not closed. This is a go\n");
    switch (event) {
    case TS_EVENT_ERROR:{
      TSDebug("jcrusher", "jcrusher_transform - event is TS_EVENT_ERROR\n");
      /* Get the write VIO for the write operation that was
         performed on ourself. This VIO contains the continuation of
         our parent transformation. */
      TSVIO upstream_vio = TSVConnWriteVIOGet(contp);
      /* Call back the write VIO continuation to let it know that we
         have completed the write operation. */
      TSContCall(TSVIOContGet(upstream_vio), TS_EVENT_ERROR, upstream_vio);
      break;
    }
    case TS_EVENT_VCONN_WRITE_COMPLETE:
      TSDebug("jcrusher", "jcrusher_transform - event is TS_EVENT_VCONN_WRITE_COMPLETE\n");
      TSVConnShutdown(TSTransformOutputVConnGet(contp), 0, 1);
      break;

    case TS_EVENT_VCONN_WRITE_READY:
      TSDebug("jcrusher", "jcrusher_transform - event is TS_EVENT_VCONN_WRITE_READY\n");
      jcrusher_handle_transform(contp);
      break;
    case TS_EVENT_IMMEDIATE:
      TSDebug("jcrusher", "jcrusher_transform - event is TS_EVENT_IMMEDIATE\n");
      jcrusher_handle_transform(contp);
      break;
    default:
      TSDebug("jcrusher", "jcrusher_transform - unknown event [%d]", event);
      jcrusher_handle_transform(contp);
      break;
    }
  }

  return 0;
}

static int
jcrusher_transformable(TSHttpTxn txnp, int server)
{
  TSMBuffer bufp;
  TSMLoc hdr_loc;
  TSMLoc field_loc;
  TSHttpStatus resp_status;
  const char *value;
  int val_length;
  int retv;

  if (server) {
    TSDebug("jcrusher", "jcrusher_transformable - Got a server request\n");
    TSHttpTxnServerRespGet(txnp, &bufp, &hdr_loc);
  } else {
    TSDebug("jcrusher", "jcrusher_transformable - Got a cached request\n");
    TSHttpTxnCachedRespGet(txnp, &bufp, &hdr_loc);
  }

  TSDebug("jcrusher", "jcrusher_transformable - About to check status code\n");
  resp_status = TSHttpHdrStatusGet(bufp, hdr_loc);
  TSDebug("jcrusher", "jcrusher_transformable - Release mloc\n");
  TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);

  //conservatively pick some statusses to compress
  if (!(resp_status == 200)) {
    TSDebug("jcrusher", "jcrusher_tranformable - http response status [%d]. No need to crush", resp_status);
    return 0;
  }

  retv = 0;

  /* We only want to do the transformation on documents that have a
     content type of "application/json". */
  TSDebug("jcrusher", "jcrusher_transformable - Check content-type header\n");
  field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, "Content-Type", 12);
  if (field_loc) {
    value = TSMimeHdrFieldValueStringGet(bufp, hdr_loc, field_loc, 0, &val_length);
    if (value && (strncasecmp(value, "application/json", sizeof("application/json") - 1) == 0)) {
      TSDebug("jcrusher", "jcrusher_transformable - Content-type is application/json\n");
      retv = 1;
    } else {
      TSDebug("jcrusher", "jcrusher_transformable - Content-type is not application/json\n");
      retv = 0;
    }
    if (TSHandleMLocRelease(bufp, hdr_loc, field_loc) == TS_ERROR) {
      TSError("[jcrusher] Error releasing MLOC while checking " "header content-type\n");
    }
  }
  if (TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc) == TS_ERROR) {
    TSError("[jcrusher] Error releasing MLOC while checking " "header status\n");
  }

  return retv;
}

static int
jcrusher_cache_transformable(TSHttpTxn txnp)
{
  int obj_status;

  if (TSHttpTxnCacheLookupStatusGet(txnp, &obj_status) == TS_ERROR) {
    TSDebug("jcrusher", "jcrusher_cache_transformable - Couldn't get cache status of object");
    return 0;
  }
  if (obj_status == TS_CACHE_LOOKUP_HIT_STALE) {
    TSDebug("jcrusher", "jcrusher_cache_transformable - Stale cache hit");
    return 0;
  }
  if (obj_status == TS_CACHE_LOOKUP_HIT_FRESH) {
    TSDebug("jcrusher", "jcrusher_cache_transformable - Fresh cache hit");
    return 1;
  }

  return 0;
}

static void
jcrusher_transform_add(TSHttpTxn txnp)
{
  TSVConn connp;
  JCrusherData *data;

  TSHttpTxnUntransformedRespCache(txnp, 0);
  TSHttpTxnTransformedRespCache(txnp, 1);

  connp = TSTransformCreate(jcrusher_transform, txnp);
  TSDebug("jcrusher", "jcrusher_transform_add - Initializing JCrusherData\n");
  data = jcrusher_data_alloc();
  data->txn = txnp;
  TSContDataSet(connp, data);
  TSDebug("jcrusher", "jcrusher_transform_add - Adding HTTP transform hook\n");
  TSHttpTxnHookAdd(txnp, TS_HTTP_RESPONSE_TRANSFORM_HOOK, connp);
  return;
}

static int
jcrusher_plugin(TSCont contp ATS_UNUSED, TSEvent event, void *edata)
{
  TSHttpTxn txnp = (TSHttpTxn) edata;

  switch (event) {
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    TSDebug("jcrusher", "jcrusher_plugin - event is TS_EVENT_HTTP_SEND_RESPONSE_HDR\n");
    if (jcrusher_cache_transformable(txnp) && jcrusher_transformable(txnp, 0)) {
      jcrusher_transform_add(txnp);
    }
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    TSDebug("jcrusher", "jcrusher_plugin - event is TS_EVENT_HTTP_READ_RESPONSE_HDR\n");
    if (jcrusher_transformable(txnp, 1)) {
      jcrusher_transform_add(txnp);
    }
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;
  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
    TSDebug("jcrusher", "jcrusher_plugin - event is TS_EVENT_HTTP_READ_RESPONSE_HDR\n");
    if (jcrusher_cache_transformable(txnp) && jcrusher_transformable(txnp, 0)) {
      jcrusher_transform_add(txnp);
    }
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;
  default:
    break;
  }

  return 0;
}

void
TSPluginInit(int argc ATS_UNUSED, const char *argv[] ATS_UNUSED)
{
  TSPluginRegistrationInfo info;
  TSMutex mutex = TS_NULL_MUTEX;

  TSDebug("jcrusher", "TSPluginInit - Start\n");

  info.plugin_name = "jcrusher";
  info.vendor_name = "BlaBlaCar";
  info.support_email = "jean-baptiste.favre@blablacar.com";

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    TSError("[jcrusher] TSPluginInit - Plugin registration failed.\n");
    goto Lerror;
  }

  /* This is call we could use if we need to protect global data */
  /* TSReleaseAssert ((mutex = TSMutexCreate()) != TS_NULL_MUTEX); */

  TSDebug("jcrusher", "TSPluginInit - Adding global hooks\n");
  TSCont jcrusher_contp = TSContCreate(jcrusher_plugin, mutex);
  TSHttpHookAdd(TS_HTTP_READ_RESPONSE_HDR_HOOK, jcrusher_contp);
  TSHttpHookAdd(TS_HTTP_SEND_RESPONSE_HDR_HOOK, jcrusher_contp);
  return;

Lerror:
  TSError("[jcrusher] TSPluginInit - Plugin disabled\n");
}
