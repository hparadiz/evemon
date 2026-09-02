/* Huawei's nvtop backend normally links libdcmi directly.  Resolve the small
 * DCMI surface used by nvtop at runtime so the GPU plugin remains loadable on
 * machines without an Ascend driver while retaining full Ascend support when
 * the driver is present. */

#include "ascend/dcmi_interface_api.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

#define DCMI_UNAVAILABLE (-1)

static pthread_once_t dcmi_once = PTHREAD_ONCE_INIT;
static void *dcmi_handle;

static void dcmi_load(void)
{
    const char *override = getenv("EVEMON_DCMI_LIBRARY");
    if (override && override[0])
        dcmi_handle = dlopen(override, RTLD_NOW | RTLD_LOCAL);
    if (!dcmi_handle)
        dcmi_handle = dlopen("libdcmi.so", RTLD_NOW | RTLD_LOCAL);
    if (!dcmi_handle)
        dcmi_handle = dlopen(
            "/usr/local/Ascend/driver/lib64/driver/libdcmi.so",
            RTLD_NOW | RTLD_LOCAL);
    if (!dcmi_handle)
        dcmi_handle = dlopen(
            "/usr/local/Ascend/driver/lib64/common/libdcmi.so",
            RTLD_NOW | RTLD_LOCAL);
}

static void *dcmi_symbol(const char *name)
{
    pthread_once(&dcmi_once, dcmi_load);
    return dcmi_handle ? dlsym(dcmi_handle, name) : NULL;
}

int dcmi_init(void)
{
    int (*fn)(void) = dcmi_symbol("dcmi_init");
    return fn ? fn() : DCMI_UNAVAILABLE;
}

int dcmi_get_card_list(int *card_num, int *card_list, int list_len)
{
    int (*fn)(int *, int *, int) = dcmi_symbol("dcmi_get_card_list");
    return fn ? fn(card_num, card_list, list_len) : DCMI_UNAVAILABLE;
}

int dcmi_get_device_num_in_card(int card_id, int *device_num)
{
    int (*fn)(int, int *) = dcmi_symbol("dcmi_get_device_num_in_card");
    return fn ? fn(card_id, device_num) : DCMI_UNAVAILABLE;
}

int dcmi_get_device_chip_info(int card_id, int device_id,
                              struct dcmi_chip_info *chip_info)
{
    int (*fn)(int, int, struct dcmi_chip_info *) =
        dcmi_symbol("dcmi_get_device_chip_info");
    return fn ? fn(card_id, device_id, chip_info) : DCMI_UNAVAILABLE;
}

int dcmi_get_device_power_info(int card_id, int device_id, int *power)
{
    int (*fn)(int, int, int *) =
        dcmi_symbol("dcmi_get_device_power_info");
    return fn ? fn(card_id, device_id, power) : DCMI_UNAVAILABLE;
}

int dcmi_get_device_temperature(int card_id, int device_id,
                                int *temperature)
{
    int (*fn)(int, int, int *) =
        dcmi_symbol("dcmi_get_device_temperature");
    return fn ? fn(card_id, device_id, temperature) : DCMI_UNAVAILABLE;
}

int dcmi_get_device_frequency(int card_id, int device_id,
                              enum dcmi_freq_type input_type,
                              unsigned int *frequency)
{
    int (*fn)(int, int, enum dcmi_freq_type, unsigned int *) =
        dcmi_symbol("dcmi_get_device_frequency");
    return fn ? fn(card_id, device_id, input_type, frequency)
              : DCMI_UNAVAILABLE;
}

int dcmi_get_device_utilization_rate(int card_id, int device_id,
                                     int input_type,
                                     unsigned int *utilization_rate)
{
    int (*fn)(int, int, int, unsigned int *) =
        dcmi_symbol("dcmi_get_device_utilization_rate");
    return fn ? fn(card_id, device_id, input_type, utilization_rate)
              : DCMI_UNAVAILABLE;
}

int dcmi_get_device_resource_info(int card_id, int device_id,
                                  struct dcmi_proc_mem_info *proc_info,
                                  int *proc_num)
{
    int (*fn)(int, int, struct dcmi_proc_mem_info *, int *) =
        dcmi_symbol("dcmi_get_device_resource_info");
    return fn ? fn(card_id, device_id, proc_info, proc_num)
              : DCMI_UNAVAILABLE;
}

int dcmi_get_hbm_info(int card_id, int device_id,
                      struct dsmi_hbm_info_stru *device_hbm_info)
{
    int (*fn)(int, int, struct dsmi_hbm_info_stru *) =
        dcmi_symbol("dcmi_get_hbm_info");
    return fn ? fn(card_id, device_id, device_hbm_info)
              : DCMI_UNAVAILABLE;
}
