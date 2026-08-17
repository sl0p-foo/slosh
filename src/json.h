/* A minimal JSON writer. No parser: we only ever emit. */
#ifndef SL0PPTY_JSON_H
#define SL0PPTY_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *buf;
  size_t len, cap;
  bool need_comma;
} json_t;

void json_init(json_t *j);
void json_free(json_t *j);

void json_obj_open(json_t *j, const char *key); /* key may be NULL in arrays */
void json_obj_close(json_t *j);
void json_arr_open(json_t *j, const char *key);
void json_arr_close(json_t *j);

void json_str(json_t *j, const char *key, const char *val, size_t len);
void json_int(json_t *j, const char *key, long long val);
void json_bool(json_t *j, const char *key, bool val);
void json_null(json_t *j, const char *key);

#endif /* SL0PPTY_JSON_H */
