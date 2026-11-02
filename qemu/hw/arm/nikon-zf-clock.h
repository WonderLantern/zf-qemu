/* SPDX-License-Identifier: GPL-2.0-or-later
 * Zf C3.01 firmware's oscillator contract: HSI16/HSE10 MHz, not a crystal
 * measurement. RCC PLLM/N/P, AHB and APB divisors. No firmware patch.
 */
static bool zf_clock_plan(uint32_t pll, uint32_t cfgr, uint32_t *hclk,
                          uint32_t *pclk1, uint32_t *pclk2)
{
    static const unsigned ahb[] = {1,1,1,1,1,1,1,1,2,4,8,16,64,128,256,512};
    static const unsigned apb[] = {1,1,1,1,2,4,8,16};
    uint64_t sys;
    switch (cfgr & 3) {
    case 0: sys = 16000000; break;
    case 1: sys = 10000000; break;
    case 2: {
        unsigned m = pll & 63, n = (pll >> 6) & 511;
        unsigned p = (((pll >> 16) & 3) + 1) * 2;
        uint64_t source = (pll & (1u << 22)) ? 10000000 : 16000000;
        if (!m || !n) return false;
        sys = source * n / (m * p);
        break;
    }
    default: return false;
    }
    if (!sys || sys > UINT32_MAX) return false;
    *hclk = sys / ahb[(cfgr >> 4) & 15];
    *pclk1 = *hclk / apb[(cfgr >> 10) & 7];
    *pclk2 = *hclk / apb[(cfgr >> 13) & 7];
    return *pclk1 && *pclk2;
}
