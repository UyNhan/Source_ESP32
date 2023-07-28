#ifndef MODBUS_CONFIG_H
#include "string.h"
#include "esp_log.h"
#include <stdint.h>
#include "sdkconfig.h"
#include "mbcontroller.h"

#pragma pack(push,1)
typedef struct 
{
    float holding_data0;
} holding_reg_params_t; 
#pragma pack(pop)

extern holding_reg_params_t holding_reg_params;

#define MB_PORT_NUM  (CONFIG_MB_UART_PORT_NUM)
#define MB_DEV_SPEED (CONFIG_MB_UART_BAUD_RATE)
#define MASTER_MAX_CIDS num_device_parameters

#define MASTER_TAG "MASTER_TEST"

#define MASTER_CHECK(a, ret_val, str, ...) \
    if (!(a)) { \
        ESP_LOGE(MASTER_TAG, "%s(%u): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
        return (ret_val); \
    }
#define HOLD_OFFSET(field) ((uint16_t)(offsetof(holding_reg_params_t, field) + 1))
#define STR(fieldname) ((const char*)( fieldname ))
// Options can be used as bit masks or parameter limits
#define OPTS(min_val, max_val, step_val) { .opt1 = min_val, .opt2 = max_val, .opt3 = step_val }

#endif 