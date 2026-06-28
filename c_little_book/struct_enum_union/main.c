#include <stdio.h>
#include "variant.h"

int main(void) {

  Variant v;

  v.type = TYPE_INT;
  v.value.i = 42;

  printf("data type is: %s\n", get_data_type(v));
  printf("data value is: ");
  switch (v.type) {
    case TYPE_INT:
      printf("%d", v.value.i);
      break;
    case TYPE_FLOAT:
      printf("%f", v.value.f);
      break;
    case TYPE_STRING:
      printf("%s", v.value.text);
      break;
  };
  printf("\n");  
  return 0;
}
