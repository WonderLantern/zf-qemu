/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Complete Zf driver configuration contracts; donor Z9 masks queried separately.
 * Generated from configuration-allowlist.json:12 source functions,1458 words.
 * This is address admission for ordinary retention, not status/IRQ/DMA behavior.
 */
#ifndef HW_ARM_NIKON_ZF_ISP199_CONFIG_H
#define HW_ARM_NIKON_ZF_ISP199_CONFIG_H
static bool zf_isp199_config_offset_verified(unsigned off)
{
    return !(off & 3) && (
           (off >= 0xb8 && off <= 0x704) ||
           (off >= 0x7e4 && off <= 0x814) ||
           (off >= 0x900 && off <= 0x14f0) ||
           (off >= 0x1a00 && off <= 0x1d3c) ||
           (off >= 0x1e00 && off <= 0x1f0c));
}
#endif
