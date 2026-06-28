typedef enum { TYPE_INT,  TYPE_FLOAT,  TYPE_STRING } DataType;

typedef union {
  int i;
  float f;
  char text[20];
} DataValue;

typedef struct {
  DataType type;
  DataValue value;
} Variant;

const char* get_data_type(Variant v);
