#ifndef DEFINES_H_
#define DEFINES_H_

#include "stm32f3xx_hal.h"

/* Typedefs */
typedef void (*void_function_ptr_type_t)(void);
typedef int32_t BOOL;

/* FW defines */
#define FW_MAJOR	0
#define FW_MINOR	1

/* Standard defines */
#define FALSE                   0
#define TRUE                    (!FALSE)
#define NULLPTR                 (void*)0

/* Macros */
#define XSTR(x)                             STR(x)
#define STR(x)                              #x
#define ARRAY_SIZE(x)                       (sizeof((x)) / sizeof((x)[0]))
#define MEMBER_SIZE(type, member)           sizeof(((type*)0)->member)
#define MEMBER_ARRAY_SIZE(type, member)     (sizeof(((type*)0)->member) / sizeof(((type*)0)->member[0]))
#define MEMBER_SUB_ARRAY_SIZE(type, member) (sizeof(((type*)0)->member[0]) / sizeof(((type*)0)->member[0][0]))

#endif
