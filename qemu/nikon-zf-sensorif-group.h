/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_NIKON_ZF_SENSORIF_GROUP_H
#define HW_ARM_NIKON_ZF_SENSORIF_GROUP_H
static inline bool zf_sensorif_group_address(hwaddr addr)
{
    return addr == 0x15083408 || addr == 0x1508340c ||
           addr == 0x15083418 || addr == 0x1508341c;
}
#endif
