#include <libwebsockets.h>
#include <jansson.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <rbus.h>
#include <signal.h>

// Global rbus handle
static rbusHandle_t g_rbusHandle = NULL;

static volatile sig_atomic_t shutdown_flag = 0;
static json_t *create_error_response(int code, const char *message, json_t *id);

// Structure to store subscription information
typedef struct {
   char *eventName; // Event name
   struct lws *wsi; // WebSocket instance
} Subscription;

#define MAX_SUBSCRIPTIONS 100
static Subscription subscriptions[MAX_SUBSCRIPTIONS];
static int subscription_count = 0;

// Signal handler for SIGTERM
static void handle_sigterm(int sig) {
   (void)sig; // Suppress unused parameter warning
   shutdown_flag = 1;
}

// Convert rbusValue_t to json_t
static json_t *rbus_value_to_json(rbusValue_t value) {
   if (!value) {
      return json_null();
   }

   rbusValueType_t type = rbusValue_GetType(value);

   switch (type) {
   case RBUS_BOOLEAN:
      return json_boolean(rbusValue_GetBoolean(value));

   case RBUS_CHAR:
      return json_integer(rbusValue_GetChar(value));

   case RBUS_BYTE:
      return json_integer(rbusValue_GetByte(value));

   case RBUS_INT8:
   case RBUS_INT16:
   case RBUS_INT32:
   case RBUS_INT64:
      return json_integer(rbusValue_GetInt64(value));

   case RBUS_UINT8:
   case RBUS_UINT16:
   case RBUS_UINT32:
   case RBUS_UINT64:
      return json_integer(rbusValue_GetUInt64(value));

   case RBUS_SINGLE:
   case RBUS_DOUBLE:
      return json_real(rbusValue_GetDouble(value));

   case RBUS_STRING: {
      const char *str = rbusValue_GetString(value, NULL);
      return str ? json_string(str) : json_null();
   }

   case RBUS_DATETIME: {
      const rbusDateTime_t *time_val = rbusValue_GetTime(value);
      if (time_val) {
         char time_str[32];
         int len = snprintf(time_str, sizeof(time_str),
            "%04d-%02d-%02dT%02d:%02d:%02d%s%02d:%02d",
            time_val->m_time.tm_year + 1900,
            time_val->m_time.tm_mon + 1,
            time_val->m_time.tm_mday,
            time_val->m_time.tm_hour,
            time_val->m_time.tm_min,
            time_val->m_time.tm_sec,
            time_val->m_tz.m_isWest ? "-" : "+",
            time_val->m_tz.m_tzhour,
            time_val->m_tz.m_tzmin);
         if (len >= (int)sizeof(time_str) || len < 0) {
            return json_null();
         }
         return json_string(time_str);
      }
      return json_null();
   }

   case RBUS_BYTES: {
      int len;
      const uint8_t *bytes = rbusValue_GetBytes(value, &len);
      if (bytes && len > 0) {
         json_t *array = json_array();
         for (int i = 0; i < len; i++) {
            json_array_append_new(array, json_integer(bytes[i]));
         }
         return array;
      }
      return json_null();
   }

   case RBUS_PROPERTY:
   case RBUS_OBJECT: {
      json_t *object = json_object();
      rbusObject_t obj = rbusValue_GetObject(value);
      if (!obj) {
         json_decref(object);
         return json_null();
      }
      rbusProperty_t prop = rbusObject_GetProperties(obj);
      while (prop) {
         const char *key = rbusProperty_GetName(prop);
         rbusValue_t val = rbusProperty_GetValue(prop);
         if (key && val) {
            json_t *json_value = rbus_value_to_json(val);
            json_object_set_new(object, key, json_value);
         }
         prop = rbusProperty_GetNext(prop);
      }
      return object;
   }

   case RBUS_NONE:
   default:
      return json_null();
   }
}

// Convert json_t to rbusValue_t
static rbusValue_t json_to_rbus_value(json_t *json) {
   if (!json) {
      return NULL;
   }

   rbusValue_t value = rbusValue_Init(NULL);

   if (json_is_boolean(json)) {
      rbusValue_SetBoolean(value, json_is_true(json));
   } else if (json_is_integer(json)) {
      rbusValue_SetInt64(value, json_integer_value(json));
   } else if (json_is_real(json)) {
      rbusValue_SetDouble(value, json_real_value(json));
   } else if (json_is_string(json)) {
      rbusValue_SetString(value, json_string_value(json));
   } else if (json_is_array(json)) {
      size_t len = json_array_size(json);
      uint8_t *bytes = malloc(len);
      if (!bytes) {
         rbusValue_Release(value);
         return NULL;
      }
      for (size_t i = 0; i < len; i++) {
         json_t *item = json_array_get(json, i);
         if (json_is_integer(item)) {
            bytes[i] = (uint8_t)json_integer_value(item);
         } else {
            free(bytes);
            rbusValue_Release(value);
            return NULL;
         }
      }
      rbusValue_SetBytes(value, bytes, len);
      free(bytes);
   } else if (json_is_object(json)) {
      rbusObject_t obj = rbusObject_Init(NULL, NULL);
      json_t *iter;
      const char *key;
      json_object_foreach(json, key, iter) {
         rbusValue_t prop_value = json_to_rbus_value(iter);
         if (prop_value) {
            rbusObject_SetValue(obj, key, prop_value);
            rbusValue_Release(prop_value);
         }
      }
      rbusValue_SetObject(value, obj);
      rbusObject_Release(obj);
   } else {
      rbusValue_Release(value);
      return NULL;
   }

   return value;
}

// Parse comma-separated paths
static char **parse_paths(const char *path_str, int *path_count) {
   if (!path_str || !*path_str) {
      *path_count = 0;
      return NULL;
   }

   int count = 1;
   const char *p = path_str;
   while (*p) {
      if (*p == ',') count++;
      p++;
   }

   char **paths = malloc(count * sizeof(char *));
   if (!paths) {
      *path_count = 0;
      return NULL;
   }

   char *path_copy = strdup(path_str);
   if (!path_copy) {
      free(paths);
      *path_count = 0;
      return NULL;
   }

   int i = 0;
   char *token = strtok(path_copy, ",");
   while (token && i < count) {
      while (*token == ' ') token++;
      char *end = token + strlen(token) - 1;
      while (end > token && *end == ' ') end--;
      *(end + 1) = '\0';
      paths[i] = strdup(token);
      if (!paths[i]) {
         for (int j = 0; j < i; j++) free(paths[j]);
         free(paths);
         free(path_copy);
         *path_count = 0;
         return NULL;
      }
      i++;
      token = strtok(NULL, ",");
   }
   *path_count = i;
   free(path_copy);
   return paths;
}

// Free parsed paths
static void free_paths(char **paths, int path_count) {
   if (paths) {
      for (int i = 0; i < path_count; i++) {
         free(paths[i]);
      }
      free(paths);
   }
}

// Perform rbus get operation for multiple paths
static json_t *rbus_get_value(rbusHandle_t handle, const char *path) {
   int path_count;
   char **paths = parse_paths(path, &path_count);
   if (!paths || path_count == 0) {
      free_paths(paths, path_count);
      return create_error_response(-32602, "Invalid or empty path", NULL);
   }

   int num_props;
   rbusProperty_t properties;
   rbusError_t err = rbus_getExt(handle, path_count, (const char **)paths, &num_props, &properties);
   if (err != RBUS_ERROR_SUCCESS) {
      free_paths(paths, path_count);
      char err_msg[256];
      snprintf(err_msg, sizeof(err_msg), "rbus_getExt failed: %s", rbusError_ToString(err));
      return create_error_response(-32000, err_msg, NULL);
   }

   json_t *result = json_object();
   rbusProperty_t prop = properties;
   while (prop) {
      const char *name = rbusProperty_GetName(prop);
      rbusValue_t value = rbusProperty_GetValue(prop);
      if (name && value) {
         json_t *json_value = rbus_value_to_json(value);
         json_object_set_new(result, name, json_value);
      }
      prop = rbusProperty_GetNext(prop);
   }

   rbusProperty_Release(properties);
   free_paths(paths, path_count);
   return result;
}

// Perform rbus set operation
static int rbus_set_value(rbusHandle_t handle, const char *path, json_t *value) {
   rbusValue_t rbus_val = json_to_rbus_value(value);
   if (!rbus_val) {
      return -1;
   }

   rbusError_t err = rbus_set(handle, path, rbus_val, NULL);
   rbusValue_Release(rbus_val);
   return err == RBUS_ERROR_SUCCESS ? 0 : -1;
}

// Event handler for rbus events
static void event_handler(rbusHandle_t handle, rbusEvent_t const *event, rbusEventSubscription_t *subscription) {
   struct lws *wsi = (struct lws *)subscription->userData;
   if (!wsi) {
      return;
   }

   // Create JSON-RPC notification
   json_t *notification = json_object();
   json_object_set_new(notification, "jsonrpc", json_string("2.0"));
   json_object_set_new(notification, "method", json_string("rbus_event"));
   json_t *params = json_object();
   json_object_set_new(params, "eventName", json_string(event->name));
   json_object_set_new(params, "type", json_string(event->type == RBUS_EVENT_VALUE_CHANGED ? "value_changed" :
      event->type == RBUS_EVENT_OBJECT_CREATED ? "object_created" :
      event->type == RBUS_EVENT_OBJECT_DELETED ? "object_deleted" :
      event->type == RBUS_EVENT_GENERAL ? "general" :
      event->type == RBUS_EVENT_INITIAL_VALUE ? "initial_value" :
      event->type == RBUS_EVENT_INTERVAL ? "interval" :
      event->type == RBUS_EVENT_DURATION_COMPLETE ? "duration_complete" : "unknown"));
   if (event->data) {
      json_t *data = rbus_value_to_json(rbusObject_GetValue(event->data, "value"));
      json_object_set_new(params, "data", data);
   } else {
      json_object_set_new(params, "data", json_null());
   }
   json_object_set_new(notification, "params", params);

   char *notification_str = json_dumps(notification, JSON_COMPACT);
   if (notification_str) {
      lws_write(wsi, (unsigned char *)notification_str, strlen(notification_str), LWS_WRITE_TEXT);
      free(notification_str);
   }
   json_decref(notification);
}

// Add subscription
static int add_subscription(const char *eventName, struct lws *wsi) {
   if (subscription_count >= MAX_SUBSCRIPTIONS) {
      return -1;
   }

   for (int i = 0; i < subscription_count; i++) {
      if (strcmp(subscriptions[i].eventName, eventName) == 0 && subscriptions[i].wsi == wsi) {
         return 0; // Subscription already exists
      }
   }

   subscriptions[subscription_count].eventName = strdup(eventName);
   if (!subscriptions[subscription_count].eventName) {
      return -1;
   }
   subscriptions[subscription_count].wsi = wsi;

   rbusEventSubscription_t sub = {
       .eventName = eventName,
       .handler = (rbusEventHandler_t)event_handler,
       .userData = wsi,
       .filter = NULL,
       .interval = 0,
       .duration = 0,
       .publishOnSubscribe = true
   };

   rbusError_t err = rbusEvent_Subscribe(g_rbusHandle, eventName, (rbusEventHandler_t)event_handler, wsi, 30);
   if (err != RBUS_ERROR_SUCCESS) {
      free(subscriptions[subscription_count].eventName);
      return -1;
   }

   subscription_count++;
   return 0;
}

// Remove subscription
static int remove_subscription(const char *eventName, struct lws *wsi) {
   for (int i = 0; i < subscription_count; i++) {
      if (strcmp(subscriptions[i].eventName, eventName) == 0 && subscriptions[i].wsi == wsi) {
         rbusEvent_Unsubscribe(g_rbusHandle, eventName);
         free(subscriptions[i].eventName);
         for (int j = i; j < subscription_count - 1; j++) {
            subscriptions[j] = subscriptions[j + 1];
         }
         subscription_count--;
         return 0;
      }
   }
   return -1;
}

// Clean up subscriptions for a closed WebSocket
static void cleanup_subscriptions(struct lws *wsi) {
   for (int i = subscription_count - 1; i >= 0; i--) {
      if (subscriptions[i].wsi == wsi) {
         rbusEvent_Unsubscribe(g_rbusHandle, subscriptions[i].eventName);
         free(subscriptions[i].eventName);
         for (int j = i; j < subscription_count - 1; j++) {
            subscriptions[j] = subscriptions[j + 1];
         }
         subscription_count--;
      }
   }
}

// JSON-RPC handling
static json_t *create_error_response(int code, const char *message, json_t *id) {
   json_t *response = json_object();
   json_object_set_new(response, "jsonrpc", json_string("2.0"));

   json_t *error = json_object();
   json_object_set_new(error, "code", json_integer(code));
   json_object_set_new(error, "message", json_string(message));
   json_object_set_new(response, "error", error);

   json_object_set(response, "id", id ? json_incref(id) : json_null());
   return response;
}

static json_t *create_success_response(json_t *result, json_t *id) {
   json_t *response = json_object();
   json_object_set_new(response, "jsonrpc", json_string("2.0"));
   json_object_set_new(response, "result", result);
   json_object_set(response, "id", id ? json_incref(id) : json_null());
   return response;
}

static json_t *handle_rbus_get(json_t *params, json_t *id) {
   const char *path = json_string_value(json_object_get(params, "path"));
   if (!path) {
      return create_error_response(-32602, "Invalid params", id);
   }

   json_t *value = rbus_get_value(g_rbusHandle, path);
   if (json_is_object(value) && json_object_get(value, "error")) {
      return value;
   }

   return create_success_response(value, id);
}

static json_t *handle_rbus_set(json_t *params, json_t *id) {
   const char *path = json_string_value(json_object_get(params, "path"));
   json_t *value = json_object_get(params, "value");
   if (!path || !value) {
      return create_error_response(-32602, "Invalid params", id);
   }

   if (rbus_set_value(g_rbusHandle, path, value) != 0) {
      return create_error_response(-32000, "Set failed", id);
   }

   return create_success_response(json_true(), id);
}

static json_t *handle_rbus_event_subscribe(json_t *params, json_t *id, struct lws *wsi) {
   const char *eventName = json_string_value(json_object_get(params, "eventName"));
   json_t *timeout_json = json_object_get(params, "timeout");
   int timeout = timeout_json && json_is_integer(timeout_json) ? (int)json_integer_value(timeout_json) : 30;

   if (!eventName) {
      return create_error_response(-32602, "Invalid params: eventName required", id);
   }

   if (add_subscription(eventName, wsi) != 0) {
      return create_error_response(-32000, "Subscription failed", id);
   }

   return create_success_response(json_true(), id);
}

static json_t *handle_rbus_event_unsubscribe(json_t *params, json_t *id, struct lws *wsi) {
   const char *eventName = json_string_value(json_object_get(params, "eventName"));
   if (!eventName) {
      return create_error_response(-32602, "Invalid params: eventName required", id);
   }

   if (remove_subscription(eventName, wsi) != 0) {
      return create_error_response(-32000, "Unsubscription failed: not subscribed", id);
   }

   return create_success_response(json_true(), id);
}

static json_t *handle_jsonrpc_request(json_t *request, struct lws *wsi) {
   json_t *id = json_object_get(request, "id");
   const char *method = json_string_value(json_object_get(request, "method"));
   json_t *params = json_object_get(request, "params");

   if (!method || !params) {
      return create_error_response(-32600, "Invalid Request", id);
   }

   if (strcmp(method, "rbus_get") == 0) {
      return handle_rbus_get(params, id);
   } else if (strcmp(method, "rbus_set") == 0) {
      return handle_rbus_set(params, id);
   } else if (strcmp(method, "rbusEvent_Subscribe") == 0) {
      return handle_rbus_event_subscribe(params, id, wsi);
   } else if (strcmp(method, "rbusEvent_Unsubscribe") == 0) {
      return handle_rbus_event_unsubscribe(params, id, wsi);
   }

   return create_error_response(-32601, "Method not found", id);
}

// Read configuration from JSON file
static int read_config(const char *filename, struct lws_context_creation_info *info) {
   json_t *root;
   json_error_t error;

   // Initialize defaults
   info->port = 8080;
   info->vhost_name = "localhost";
   info->options = 0;

   // Load JSON file
   root = json_load_file(filename, 0, &error);
   if (!root) {
      fprintf(stderr, "Warning: Failed to load config file %s: %s\n", filename, error.text);
      return -1;
   }

   // Parse host
   json_t *host = json_object_get(root, "host");
   if (json_is_string(host)) {
      info->vhost_name = strdup(json_string_value(host));
      if (!info->vhost_name) {
         fprintf(stderr, "Error: Failed to allocate memory for host\n");
         json_decref(root);
         return -1;
      }
   }

   // Parse port
   json_t *port = json_object_get(root, "port");
   if (json_is_integer(port)) {
      info->port = (int)json_integer_value(port);
      if (info->port < 0 || info->port > 65535) {
         fprintf(stderr, "Warning: Invalid port %d in config, using default 8080\n", info->port);
         info->port = 8080;
      }
   }

   // Parse ssl_enabled
   json_t *ssl_enabled = json_object_get(root, "ssl_enabled");
   if (json_is_boolean(ssl_enabled)) {
      if (json_is_true(ssl_enabled)) {
         info->options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
      }
   }

   json_decref(root);
   return 0;
}

// WebSocket handling
static int callback_jsonrpc(struct lws *wsi, enum lws_callback_reasons reason,
   void *user, void *in, size_t len) {
   switch (reason) {
   case LWS_CALLBACK_RECEIVE: {
      char *buffer = malloc(len + 1);
      if (!buffer) {
         json_t *response = create_error_response(-32000, "Memory allocation failed", NULL);
         char *response_str = json_dumps(response, JSON_COMPACT);
         if (response_str) {
            lws_write(wsi, (unsigned char *)response_str, strlen(response_str), LWS_WRITE_TEXT);
            free(response_str);
         }
         json_decref(response);
         return -1;
      }
      memcpy(buffer, in, len);
      buffer[len] = '\0';

      json_error_t error;
      json_t *request = json_loads(buffer, 0, &error);
      free(buffer);

      if (!request) {
         json_t *response = create_error_response(-32700, "Parse error", NULL);
         char *response_str = json_dumps(response, JSON_COMPACT);
         if (response_str) {
            lws_write(wsi, (unsigned char *)response_str, strlen(response_str), LWS_WRITE_TEXT);
            free(response_str);
         }
         json_decref(response);
         break;
      }

      json_t *response = handle_jsonrpc_request(request, wsi);
      char *response_str = json_dumps(response, JSON_COMPACT);
      if (response_str) {
         lws_write(wsi, (unsigned char *)response_str, strlen(response_str), LWS_WRITE_TEXT);
         free(response_str);
      } else {
         lws_write(wsi, (unsigned char *)"{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"Response serialization failed\"},\"id\":null}",
            strlen("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"Response serialization failed\"},\"id\":null}"),
            LWS_WRITE_TEXT);
      }
      json_decref(request);
      json_decref(response);
      break;
   }
   case LWS_CALLBACK_CLOSED: {
      cleanup_subscriptions(wsi);
      break;
   }
   default:
      break;
   }

   return 0;
}

static struct lws_protocols protocols[] = {
    {
        "jsonrpc",
        callback_jsonrpc,
        0,
        4096,
    },
    { NULL, NULL, 0, 0 }
};

int main(int argc, char *argv[]) {
   // Configure rbus logging
   rbus_setLogLevel(RBUS_LOG_ERROR);

   // Set up SIGTERM handler
   signal(SIGTERM, handle_sigterm);

   // Initialize rbus
   if (rbus_open(&g_rbusHandle, "rbus-jsonrpc") != RBUS_ERROR_SUCCESS) {
      fprintf(stderr, "Error: failed to open rbus handle\n");
      return 1;
   }

   // Set libwebsockets log level
   lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

   // Parse command-line arguments
   const char *config_file = "config.json";
   const char *host = NULL;
   int port = 0;
   int i = 1;
   while (i < argc) {
      if (strcmp(argv[i], "-c") == 0) {
         if (i + 1 >= argc) {
            fprintf(stderr, "Error: -c requires a configuration file path\n");
            rbus_close(g_rbusHandle);
            return 1;
         }
         config_file = argv[i + 1];
         i += 2;
      } else if (!host) {
         host = argv[i];
         i++;
      } else if (!port) {
         port = atoi(argv[i]);
         if (port < 0 || port > 65535) {
            fprintf(stderr, "Error: Invalid port %d\n", port);
            rbus_close(g_rbusHandle);
            return 1;
         }
         i++;
      } else {
         fprintf(stderr, "Error: Unknown argument %s\n", argv[i]);
         rbus_close(g_rbusHandle);
         return 1;
      }
   }

   // Read configuration file
   struct lws_context_creation_info info = {0};
   if (read_config(config_file, &info) != 0) {
      fprintf(stderr, "Warning: Using default configuration\n");
      info.port = 8080;
      info.vhost_name = "localhost";
      info.options = 0;
   }

   // Override with command-line arguments if provided
   if (host) {
      if (info.vhost_name) free((char *)info.vhost_name);
      info.vhost_name = strdup(host);
      if (!info.vhost_name) {
         fprintf(stderr, "Error: Failed to allocate memory for host\n");
         rbus_close(g_rbusHandle);
         return 1;
      }
   }
   if (port) {
      info.port = port;
      if (info.port < 0 || info.port > 65535) {
         fprintf(stderr, "Error: Invalid port %d from command line\n", info.port);
         if (info.vhost_name) free((char *)info.vhost_name);
         rbus_close(g_rbusHandle);
         return 1;
      }
   }

   // Set protocols
   info.protocols = protocols;

   struct lws_context *context = lws_create_context(&info);
   if (!context) {
      fprintf(stderr, "lws init failed\n");
      if (info.vhost_name) free((char *)info.vhost_name);
      rbus_close(g_rbusHandle);
      return 1;
   }

   printf("JSON-RPC WebSocket server running on ws://%s:%d\n", info.vhost_name, info.port);

   // Main event loop with shutdown check
   while (!shutdown_flag) {
      lws_service(context, 1000);
   }

   printf("Received SIGTERM, shutting down...\n");

   // Cleanup
   for (int i = 0; i < subscription_count; i++) {
      rbusEvent_Unsubscribe(g_rbusHandle, subscriptions[i].eventName);
      free(subscriptions[i].eventName);
   }
   subscription_count = 0;

   lws_context_destroy(context);
   if (info.vhost_name) free((char *)info.vhost_name);
   rbus_close(g_rbusHandle);

   printf("Server shutdown complete\n");
   return 0;
}