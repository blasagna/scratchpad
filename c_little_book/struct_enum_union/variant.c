#include "variant.h"

const char* get_data_type(Variant v) {
  switch (v.type) {
    case TYPE_INT:
      return "int";
    case TYPE_FLOAT:
      return "float";
    case TYPE_STRING:
      return "string";
  };
  return "unknown";
}
