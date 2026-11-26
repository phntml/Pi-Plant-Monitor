#include "msg.h"

#include <stddef.h>

const char *sensor_kind_name(uint32_t kind)
{
    switch (kind) {
    case SENSOR_LIGHT: return "light";
    case SENSOR_SOIL:  return "soil";
    }
    return NULL;
}
