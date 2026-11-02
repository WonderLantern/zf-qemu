/*
 * Nikon Z 6III -- ../z9-qemu's capture models, in front of the board's own
 * register windows.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * hw/arm/nikon-z63-z9lab.c is ../z9-qemu's Z 9 board file (WonderLantern;
 * GPL-2.0-or-later; revision deb3a75), carried in by
 * z63-tools/import_z9lab.py with the edits that script lists, plus its
 * model headers: nikon-z9-pixels.h with the live-view edits the script
 * lists, the rest unchanged (nikon-raw-pixels.h, and the capture path's
 * nikon-z9-capture-nv16.h, -d980.h, -jpeg.h, -thumbnail-nv16.h, -pcie.h;
 * the JPEG encoder links libjpeg). The Z 9 reached its live view with those
 * models: the sensor-interface event block at 0x15080000 with a periodic
 * frame-boundary input, the ISP_APB configuration/submission blocks
 * (0x194, 0x19b/0x19c, 0x19d), engines 0x150c/0x150d/0x1524 (+MVD), 0x1543,
 * 0x151/0x152, the 0x154c stream stops, statistics, and a functional RAW ->
 * NV12/NV16 pipeline fed from a local Bayer file.
 *
 * How it is attached. With -machine z9-capture=on the board creates that
 * object with the switches below and offers it every access to a register
 * window or to unmapped space inside the capture ranges (z63_z9lab_range).
 * Where the Z 9 model answers, its answer is used; where it declines -- its
 * strict "unknown access" stop, which the import turns into a miss -- the
 * board's own window answers or stops exactly as before. Writes the model
 * takes are still stored by the window, so a later read it declines sees
 * them. The display (0x1548xxxx), graphics (0x1554xxxx) and every other
 * block with its own MemoryRegionOps never reach it.
 *
 * What that claims. The Z 9 model was written against the Z 9 image; this
 * image is the same EXPEED 7 generation, and the parts checked so far agree
 * (the capture interrupt names, the BUFFER_EXT image banks at 0x840000000,
 * the sensor-interface base). Everything else is an assumption to be
 * checked as the firmware reaches it, which is why the first access to
 * each 4 KiB page the model answers is named on stderr, with the PC. Off by
 * default.
 *
 * The switches are ../z9-qemu's tools/model-properties.json less the ones
 * for blocks this board models itself (the attachment refuses those; see
 * the end of nikon-z63-z9lab.c), plus z63-boundary-ms=33: the periodic
 * frame-boundary input ../z9-qemu's run loop delivered from outside (its
 * tools/z9_global1_boundary_input.py), done here by a timer. Two switches
 * the Z 9 profile leaves off are on here, because this image reaches what
 * they model: zr-primary-bank1-model, engine 0x150c's second status bank
 * on INTID 0x107, which the front-end start waits on (the start
 * acknowledgement in nikon-z63-z9lab.c's attachment section explains what
 * raises it); and event1508-aux-config-model, plain retention of the SDES
 * channel words +0/+4/+8/+0xc in both banks, which live-view start writes
 * read-modify-write (sub_40C2ADEC: bank 1 +0 = 0x10001) and which nothing
 * else on the board answers. And one input: event1508-aux-status-input
 * with eight zero words, which makes the SDES error-status words (+0x24,
 * +0x110, +0x118, +0x11c of both banks; W1C) readable as "no error" --
 * the link to the virtual sensor is healthy -- instead of not answering;
 * live-view start reads bank 1 +0x24 (sub_40C2B218). Also this board's
 * z63-150c-frame-end: engine 0x150c's primary path ending its frame at the
 * boundary on its own while the combined RAW pipeline is not engaged (see
 * the attachment section), and z63-150c-path2-end: +0x210 bit 31, the
 * frame end this board reads as engine bit 2's, which the live-view
 * module's lane 2 waits for (attachment section), and z63-sensor-vd: the
 * sensor's frame boundary (channel bit 0, global0 bit 7) carried on while capture group 0
 * is off, which the sensor-mode change in the middle of live view waits for
 * before it turns the group back on (attachment section). A third switch the Z 9
 * profile leaves off is on: engine150c-bit2-model, the model's own
 * "primary bit2 input at the lab clock boundary" (+0x204 bit 2, while
 * engine bit 1 is active). Its note says the real callback maps it to
 * 0x41d0001, and in this image it does: the front end's sub_410AB2F8
 * counts +0x204 bit 2 (0x410ab35c) and then sends 0x41d0001 to its type-0
 * consumers (0x410abe28), among them the live-view module's sub_40E69B78,
 * which sets flag 1 of 0xa03d22008 -- what lane 0 of each live-view frame
 * job waits for in its second stage (sub_40E75140, no timeout). Nothing
 * else raised bit 2, and the first frame job never finished.
 * And z63-1508-lane0-ack, with the z9 model's
 * event1508-global0-bit10-model for its W1C: the sensor interface's lane-0
 * request (+0x38 = 1) acknowledged on global0 bit 10, which the sensor
 * module's stop before a live-view restart waits 150 ticks for (camera
 * error 0x40 on timeout; see the attachment section). After those come
 * ../z9-qemu's capture models, its capture profile's list, and
 * z63-still-capture: a release takes the still through engine 0x150c, the
 * D980 writer, 0x153, 0x151, the 0x1543 NV16 outputs and the JPEG cores,
 * and the camera's own record path writes DCIM/xxxNZ6_3/DSC_nnnn.JPG to
 * the card (-machine cfexpress=on and an NVMe drive on dw-pcie). It does
 * so with the shutter type (custom setting d6) at "electronic shutter";
 * at "auto", "mechanical" and "electronic front-curtain" the image takes a
 * path this board does not complete yet (git log, "the still's capture
 * job ends"). -machine z9-capture-props= replaces the list
 * (items separated by ';'). The statistics and RAW models need an
 * NZ9RAW1 Bayer file: -machine z9-capture-raw=<path>; z63-tools/make_raw.py
 * writes a synthetic one. That picture is an input chosen for the run, not
 * anything the camera's sensor produces.
 */

#ifndef HW_ARM_NIKON_Z63_Z9LAB_H
#define HW_ARM_NIKON_Z63_Z9LAB_H

Object *z63_z9lab_new(Object *owner, DeviceState *gic, uint64_t ram_size,
                      MemoryRegion *dram, MemoryRegion *buffer_ext,
                      const char *props, Error **errp);
bool z63_z9lab_read(Object *obj, hwaddr addr, unsigned size,
                    uint64_t *value);
bool z63_z9lab_write(Object *obj, hwaddr addr, uint64_t value,
                     unsigned size);
uint64_t z63_z9lab_misses(Object *obj);
uint32_t z63_z9lab_isp199_config_mask(unsigned off);
#include "hw/arm/nikon-zf-sensorif-aux-config.h"
bool z63_z9lab_zf_aux_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_aux_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_aux_profile_valid(Object *obj, bool group, bool primary, uint64_t source_period_ns);
#include "hw/arm/nikon-zf-sensorif-output-config.h"
bool z63_z9lab_zf_output_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_output_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_output_profile_valid(Object *obj, bool group, bool primary, uint64_t source_period_ns, bool source_stop);
char *z63_z9lab_zf_output_state(Object *obj, Error **errp);
#include "hw/arm/nikon-zf-sensorif-group.h"
bool z63_z9lab_zf_group_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_group_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_group_profile_valid(Object *obj, bool primary, uint64_t source_period_ns, bool source_stop);
#include "hw/arm/nikon-zf-sensorif-primary.h"
bool z63_z9lab_zf_primary_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_primary_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_primary_profile_valid(Object *obj, uint64_t source_period_ns, bool source_stop);
char *z63_z9lab_zf_primary_state(Object *obj, Error **errp);
#include "hw/arm/nikon-zf-isp1a3-config.h"
bool z63_z9lab_zf_isp1a3_profile_valid(Object *obj);
char *z63_z9lab_zf_isp1a3_state(Object *obj, Error **errp);
#include "hw/arm/nikon-zf-isp199-owner.h"
bool z63_z9lab_zf_isp199_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value, bool diagnostic);
bool z63_z9lab_zf_isp199_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_isp199_profile_valid(Object *obj);
char *z63_z9lab_zf_isp199_state(Object *obj, Error **errp);
static bool z63_ramdmp_load(const char *block, hwaddr addr, unsigned size);
static bool zf_isp199_diagnostic_read(hwaddr addr, unsigned size, uint64_t *value);
#include "hw/arm/nikon-zf-stream154c.h"
bool z63_z9lab_zf_stream154c_profile_valid(Object *obj);
bool z63_z9lab_zf_stream154c_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
char *z63_z9lab_zf_stream154c_state(Object *obj, Error **errp);
#include "hw/arm/nikon-zf-isp192-owner.h"
bool z63_z9lab_zf_isp192_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value, bool diagnostic);
bool z63_z9lab_zf_isp192_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_isp192_profile_valid(Object *obj);
char *z63_z9lab_zf_isp192_state(Object *obj, Error **errp);
static bool zf_isp192_diagnostic_read(hwaddr addr, unsigned size, uint64_t *value);
#include "hw/arm/nikon-zf-isp-small-owner.h"
bool z63_z9lab_zf_isp_small_read(Object *obj, hwaddr addr, unsigned size, uint64_t *value);
bool z63_z9lab_zf_isp_small_write(Object *obj, hwaddr addr, uint64_t value, unsigned size);
bool z63_z9lab_zf_isp_small_profile_valid(Object *obj);
char *z63_z9lab_zf_isp_small_state(Object *obj, Error **errp);

static const char z63_z9lab_default_props[] =
    "bootstrap-fixtures=on;"
    "image-bank-model=on;"
    "engine150c-control-model=on;"
    "event1508-front-model=on;"
    "event1508-aux-model=on;"
    "event1508-aux1-model=on;"
    "event1508-aux-config-model=on;"
    "event1508-aux-status-input=0:0:0:0:0:0:0:0;"
    "event1508-channel-model=on;"
    "event1508-timing-model=on;"
    "event1508-timing-period-ns=33333333;"
    "event1508-global1-model=on;"
    "event1508-global0-model=on;"
    "event1508-global0-paired-boundary=on;"
    "config1524-model=on;"
    "primary-metadata14c-input=1;"
    "primary19d-config-model=on;"
    "primary19bc-config-model=on;"
    "primary194-config-model=on;"
    "primary194-submit-model=on;"
    "primary19bc-submit-model=on;"
    "primary19d-submit-model=on;"
    "config1524-control100-input=0;"
    "engine1524-control-model=on;"
    "engine1524-mvd-model=on;"
    "event1508-group-model=on;"
    "config150d-model=on;"
    "secondary150d-submit-model=on;"
    "primary-parameter-control-model=on;"
    "task1500-control-model=on;"
    "engine1543-control-model=on;"
    "engine151-control-model=on;"
    "engine152-control-model=on;"
    "primary19bc-zero-results-input=on;"
    "pipeline-aux-zero-input=on;"
    "raw-pipeline-model=on;"
    "raw-pipeline-1514-completion-model=on;"
    "engine150d-control-model=on;"
    "engine150c-stop-model=on;"
    "zr-primary-bank1-model=on;"
    "statistics-model=on;"
    "primary-empty-end-model=on;"
    "engine150c-bit2-model=on;"
    "stream154c-stop-model=on;"
    "event1508-stop-model=on;"
    "engine150d-readback-input=0:0:0:0:0;"
    "multi-io-counter-model=on;"
    "event1508-global0-bit10-model=on;"
    "event1508-global0-bit26-model=on;"
    "z63-boundary-ms=33;"
    "z63-150c-frame-end=on;"
    "z63-150c-path2-end=on;"
    "z63-sensor-vd=on;"
    "z63-1508-lane0-ack=on;"
    /* The still: ../z9-qemu's capture models (its capture profile's list)
     * and z63-still-capture (attachment section). */
    "event1508-secondary-timing-model=on;"
    "primary-d980-config-model=on;"
    "primary-d980-stream-model=on;"
    "engine152c-config-model=on;"
    "engine152c-request-model=on;"
    "engine151-raw-output-model=on;"
    "engine1544-config-model=on;"
    "engine1544-neutral-copy-model=on;"
    "engine1540-descriptors-model=on;"
    "engine1540-parameters-model=on;"
    "engine1540-control-model=on;"
    "engine1540-frame-pass-model=on;"
    "primary-stopped-source-reset-model=on;"
    "primary-reset-parameter-retention-model=on;"
    "primary20-empty-reset-model=on;"
    "primary-d980-packed14-model=on;"
    "primary-d980-single-frame-model=on;"
    "primary-d980-source-notify-model=on;"
    "raw-source-boundary-model=on;"
    "primary-d980-prepared-start-model=on;"
    "primary-d980-channel-boundary-model=on;"
    "primary-capture-direct-stop-model=on;"
    "engine1543-capture-control-model=on;"
    "engine15e2-config-model=on;"
    "engine153-config-model=on;"
    "engine153-inplace-identity-model=on;"
    "engine151-capture-pixels-model=on;"
    "engine151-average-result-model=on;"
    "engine15e2-jpeg-model=on;"
    "engine15e2-deferred-input-model=on;"
    "engine15e2-limit-disabled-model=on;"
    "engine15e2-result474-model=on;"
    "engine15e2-preflight-model=on;"
    "capture-nv16-pipeline-model=on;"
    "engine1542-ram-input-control-model=on;"
    "capture-thumbnail-nv16-model=on;"
    "z63-still-capture=on;"
    /* Playback: the JPEG cores' decode mode (attachment section). */
    "z63-jpeg-decode=on";

#define Z63_Z9LAB_LO 0x15000000ULL
#define Z63_Z9LAB_HI 0x16100000ULL
/* The multi-IO timer block, ../z9-qemu's multi-io-counter-model: channel
 * interrupt registers at 0x0e000100 + n * 0x100, the global enables at
 * 0x0e001000.. and channel configuration at 0x0e001100 + n * 0x100. The
 * sensor power-up sequence (sub_4115EDF4) times each of its steps on
 * channel 5 and waits for its interrupt without a timeout. */
#define Z63_Z9LAB_MIO_LO 0x0e000000ULL
#define Z63_Z9LAB_MIO_HI 0x0e002000ULL
/* The PCIe ports' link wrappers (../z9-qemu's pcie_wrapper_word, link
 * input +0xb4 and legacy-interrupt status +0xe8), with -machine
 * cfexpress=on only. */
#define Z63_Z9LAB_PCIEW_LO 0x17b00000ULL
#define Z63_Z9LAB_PCIEW_HI 0x17c00000ULL
/* The video reset and peripheral-select block (../zr-qemu's
 * video-initial-reset, video-nr-reset and video-peripheral-control models:
 * the seven-bit hold/release bundle at 0x0d154348/0x0d154248, video reset
 * 15 and the NR mask at 0x0d154240/0x0d154340, and the peripheral
 * selection at 0x0d154150 with its acknowledgement at 0x0d154030). This
 * board has a reset window of its own over the same page; the model is
 * asked first and declines everything but the offsets above, so the
 * window still answers the rest. Without this, a movie recording stops
 * the machine at the second step of its start sequence. */
#define Z63_Z9LAB_VRES_LO 0x0d154000ULL
#define Z63_Z9LAB_VRES_HI 0x0d155000ULL
#define Z63_Z9LAB_PAGES  (((Z63_Z9LAB_HI - Z63_Z9LAB_LO) >> 12) + \
                          ((Z63_Z9LAB_MIO_HI - Z63_Z9LAB_MIO_LO) >> 12) + \
                          ((Z63_Z9LAB_VRES_HI - Z63_Z9LAB_VRES_LO) >> 12) + \
                          ((Z63_Z9LAB_PCIEW_HI - Z63_Z9LAB_PCIEW_LO) >> 12))

static Object *z63_z9lab;   /* NULL unless -machine z9-capture=on */
static bool zf_mio_only; /* restricted Zf profile, MIO plus explicit sub-blocks */
static bool zf_aux_enabled; /* AUX is never an implicit consequence of MIO */
static bool zf_output_enabled; /* explicit complete configuration island */
static bool zf_group_enabled; /* explicit no-source request model */
static bool zf_primary_enabled; /* one native control/FRONT owner, no source */
static bool zf_isp1a3_enabled; /* closed own configuration domain */
static bool zf_isp199_enabled; /* complete pair, no board fallback */
static bool zf_stream154c_enabled; /* three channels, explicit empty-only assumption */
static bool zf_isp192_enabled; /* closed pair of complete ordinary domains */
static bool zf_isp_small_enabled; /* three complete ordinary domains */
static bool zf_statistics_enabled;
static bool zf_still_enabled;
static bool zf_video_codec_enabled; /* opt-in downstream controller ownership */
int z63_z9lab_zf_frame_gpio_level(Object *obj);
void z63_z9lab_zf_capture_gpio(Object *obj, uint16_t previous, uint16_t value);
static bool z63_z9lab_pcie; /* -machine cfexpress=on: the PCIe port-2 models */
static unsigned long *z63_z9lab_named;

/* The capture ranges, less the display unit/complex (0x15480000..
 * 0x154bffff) and graphics (0x15540000..0x1555ffff), which are this
 * board's. */
static bool z63_z9lab_range(hwaddr addr)
{
    if (addr >= Z63_Z9LAB_MIO_LO && addr < Z63_Z9LAB_MIO_HI) {
        return true;
    }
    if (zf_mio_only) {
        return (zf_aux_enabled && zf_sensorif_aux_address_verified(addr)) ||
               (zf_output_enabled && zf_sensorif_output_address_verified(addr)) ||
               (zf_group_enabled && zf_sensorif_group_address(addr)) ||
               (zf_primary_enabled && zf_sensorif_primary_address(addr)) ||
               (zf_isp1a3_enabled && zf_isp1a3_config_address(addr)) ||
               (zf_video_codec_enabled && addr >= 0x16010000 && addr < 0x16012000) ||
               (zf_video_codec_enabled && addr >= 0x16018000 && addr < 0x1601a300);
    }
    if (z63_z9lab_pcie && addr >= Z63_Z9LAB_PCIEW_LO &&
        addr < Z63_Z9LAB_PCIEW_HI) {
        return true;                /* the PCIe link wrappers */
    }
    if (addr >= Z63_Z9LAB_VRES_LO && addr < Z63_Z9LAB_VRES_HI) {
        return true;                /* video reset / peripheral select */
    }
    return addr >= Z63_Z9LAB_LO && addr < Z63_Z9LAB_HI &&
           !(addr >= 0x15480000 && addr < 0x154c0000) &&
           !(addr >= 0x15540000 && addr < 0x15560000);
}

static void z63_z9lab_name(hwaddr addr, bool write, uint64_t value)
{
    unsigned capture = (Z63_Z9LAB_HI - Z63_Z9LAB_LO) >> 12;
    unsigned mio = (Z63_Z9LAB_MIO_HI - Z63_Z9LAB_MIO_LO) >> 12;
    unsigned vres = (Z63_Z9LAB_VRES_HI - Z63_Z9LAB_VRES_LO) >> 12;
    unsigned page = addr >= Z63_Z9LAB_PCIEW_LO ?
                        capture + mio + vres +
                        ((addr - Z63_Z9LAB_PCIEW_LO) >> 12) :
                    addr >= Z63_Z9LAB_LO ? (addr - Z63_Z9LAB_LO) >> 12 :
                    addr >= Z63_Z9LAB_MIO_LO ?
                        capture + ((addr - Z63_Z9LAB_MIO_LO) >> 12) :
                    capture + mio + ((addr - Z63_Z9LAB_VRES_LO) >> 12);

    if (!test_and_set_bit(page, z63_z9lab_named)) {
        error_report("Z63_Z9LAB: page 0x%" HWADDR_PRIx " answered by "
                     "../z9-qemu's model (first: %s 0x%" HWADDR_PRIx
                     " = 0x%" PRIx64 ", pc=0x%" PRIx64 ")",
                     addr & ~(hwaddr)0xfff, write ? "write" : "read", addr,
                     value, current_cpu ? ARM_CPU(current_cpu)->env.pc : 0);
    }
}

static void zf_block_reject(const char *block, hwaddr addr, unsigned size, bool write, uint64_t value)
{
    error_report("ZF %s unsupported %s addr=0x%" HWADDR_PRIx " size=%u value=0x%"
                 PRIx64 " pc=0x%" PRIx64, block, write ? "write" : "read", addr, size,
                 value, current_cpu ? ARM_CPU(current_cpu)->env.pc : 0);
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
}

static bool zf_isp199_owns_address(hwaddr addr)
{
    return zf_isp199_enabled && zf_isp199_aperture(addr);
}

static bool zf_stream154c_owns_address(hwaddr addr)
{
    return zf_stream154c_enabled && zf_stream154c_aperture(addr);
}

static bool zf_isp192_owns_address(hwaddr addr)
{
    return zf_isp192_enabled && zf_isp192_aperture(addr);
}

static bool zf_isp_small_owns_address(hwaddr addr)
{
    return zf_isp_small_enabled && zf_isp_small_aperture(addr);
}

/* Controller and both config150d banks use the donor's full handlers.
 * Unimplemented config readback remains in the board's declared storage.
 * A declined controller operation is named by z63-miss-log for the next fix. */
static bool zf_video_raw_owns_address(hwaddr addr)
{
    return zf_video_codec_enabled &&
        ((addr >= 0x152c0000 && addr < 0x152c1000) ||
         (addr >= 0x15300000 && addr < 0x15301000) ||
         (addr >= 0x15ac0000 && addr < 0x15ac00c0) ||
         (addr >= 0x15b00000 && addr < 0x15b000b0));
}

static bool zf_statistics_owns_address(hwaddr addr)
{
    if (zf_video_raw_owns_address(addr)) { return true; }
    if (zf_video_codec_enabled &&
        (addr >= 0x15640000 && addr < 0x15662000)) {
        return true;
    }
    if (zf_still_enabled && addr >= 0x15e20000 && addr < 0x15e30000) {
        return true;
    }
    if (zf_isp199_owns_address(addr) || zf_isp192_owns_address(addr) ||
        zf_isp_small_owns_address(addr)) {
        return false;
    }
    return zf_statistics_enabled &&
           (((addr >= 0x15080000 && addr < 0x15084000) &&
             !zf_sensorif_primary_address(addr) && !zf_sensorif_group_address(addr) &&
             !zf_sensorif_aux_address_verified(addr) && !zf_sensorif_output_address_verified(addr)) ||
            (addr >= 0x150c0000 && addr < 0x150da000) ||
            (addr >= 0x150d0000 && addr < 0x150d4000) ||
            (addr >= 0x150d6000 && addr < 0x150d7604) ||
            (addr >= 0x150d8000 && addr < 0x150d9504) ||
            (addr >= 0x15100000 && addr < 0x15480000));
}

static bool z63_front_read(hwaddr addr, unsigned size, uint64_t *value)
{
    if (zf_video_codec_enabled && addr >= 0x16010000 && addr < 0x16012000) {
        *value = 0;
        if (!z63_z9lab || !z63_z9lab_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("WAVE5", addr, size, false, 0);
        }
        return true;
    }
    if (zf_video_raw_owns_address(addr)) {
        *value = 0;
        if (!z63_z9lab_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("VIDEO_RAW", addr, size, false, 0);
        }
        return true;
    }
    if (zf_still_enabled && size == 4 &&
        (addr == 0x0d154030 || addr == 0x0d154150)) {
        return z63_z9lab_read(z63_z9lab, addr, size, value);
    }
    if (zf_statistics_enabled && addr >= 0x15c00000 && addr < 0x15c00c00) {
        return z63_z9lab_read(z63_z9lab, addr, size, value);
    }
    if (zf_statistics_owns_address(addr)) {
        return z63_z9lab_read(z63_z9lab, addr, size, value);
    }
    if (zf_isp_small_owns_address(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_isp_small_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("ISP_SMALL", addr, size, false, 0);
        }
        return true;
    }
    if (zf_isp192_owns_address(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_isp192_read(z63_z9lab, addr, size, value, false) &&
            !zf_isp192_diagnostic_read(addr, size, value) &&
            !z63_ramdmp_load("ISP192", addr, size)) {
            zf_block_reject("ISP192", addr, size, false, 0);
        }
        return true;
    }
    if (zf_stream154c_owns_address(addr)) {
        *value = 0;
        if (size != 4 || !zf_stream154c_read_address(addr) ||
            !z63_z9lab_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("STREAM154C", addr, size, false, 0);
        }
        return true;
    }
    if (zf_isp199_enabled && zf_isp199_aperture(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_isp199_read(z63_z9lab, addr, size, value, false) &&
            !zf_isp199_diagnostic_read(addr, size, value) &&
            !z63_ramdmp_load("ISP199", addr, size)) {
            zf_block_reject("ISP199", addr, size, false, 0);
        }
        return true;
    }
    if (zf_isp1a3_enabled && zf_isp1a3_config_address(addr)) {
        *value = 0;
        if (size != 4 || !z63_z9lab_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("ISP1A3", addr, size, false, 0);
        }
        return true;
    }
    if (zf_primary_enabled && zf_sensorif_primary_address(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_primary_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("PRIMARY", addr, size, false, 0);
        }
        return true;
    }
    if (zf_group_enabled && zf_sensorif_group_address(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_group_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("GROUP", addr, size, false, 0);
        }
        return true;
    }
    if (zf_output_enabled && zf_sensorif_output_address_verified(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_output_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("OUTPUT", addr, size, false, 0);
        }
        return true;
    }
    if (zf_aux_enabled && zf_sensorif_aux_address_verified(addr)) {
        *value = 0;
        if (!z63_z9lab_zf_aux_read(z63_z9lab, addr, size, value)) {
            zf_block_reject("AUX", addr, size, false, 0);
        }
        return true;
    }
    if (!z63_z9lab || !z63_z9lab_range(addr) ||
        !z63_z9lab_read(z63_z9lab, addr, size, value)) {
        return false;
    }
    z63_z9lab_name(addr, false, *value);
    return true;
}

static bool z63_front_write(hwaddr addr, uint64_t value, unsigned size)
{
    if (zf_video_codec_enabled && addr >= 0x16010000 && addr < 0x16012000) {
        if (!z63_z9lab || !z63_z9lab_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("WAVE5", addr, size, true, value);
        }
        return true;
    }
    if (zf_video_codec_enabled && addr >= 0x16018000 && addr < 0x1601a300) {
        if (!z63_z9lab_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("VIDEO_PROCESSING", addr, size, true, value);
        }
        return true;
    }
    if (zf_video_raw_owns_address(addr)) {
        if (!z63_z9lab_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("VIDEO_RAW", addr, size, true, value);
        }
        return true;
    }
    if (zf_still_enabled && size == 4 &&
        (addr == 0x0d154030 || addr == 0x0d154150)) {
        return z63_z9lab_write(z63_z9lab, addr, value, size);
    }
    if (zf_still_enabled && size == 4 &&
        (addr == 0x0d154240 || addr == 0x0d154244 || addr == 0x0d154248 ||
         addr == 0x0d154340 || addr == 0x0d154344 || addr == 0x0d154348)) {
        /* Fan out the six reset banks; board owns system clock/readback. */
        z63_z9lab_write(z63_z9lab, addr, value, size);
    }
    if (zf_statistics_enabled && size == 4 && value == 0x400 &&
        (addr == 0x0d154340 || addr == 0x0d154240)) {
        return z63_z9lab_write(z63_z9lab, addr, value, size);
    }
    if (zf_statistics_owns_address(addr)) {
        return z63_z9lab_write(z63_z9lab, addr, value, size);
    }
    if (zf_isp_small_owns_address(addr)) {
        if (!z63_z9lab_zf_isp_small_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("ISP_SMALL", addr, size, true, value);
        }
        return true;
    }
    if (zf_isp192_owns_address(addr)) {
        if (!z63_z9lab_zf_isp192_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("ISP192", addr, size, true, value);
        }
        return true;
    }
    if (zf_stream154c_owns_address(addr)) {
        if (!z63_z9lab_zf_stream154c_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("STREAM154C", addr, size, true, value);
        }
        return true;
    }
    if (zf_isp199_enabled && zf_isp199_aperture(addr)) {
        if (!z63_z9lab_zf_isp199_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("ISP199", addr, size, true, value);
        }
        return true;
    }
    if (zf_isp1a3_enabled && zf_isp1a3_config_address(addr)) {
        if (size != 4 || value > UINT32_MAX ||
            !z63_z9lab_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("ISP1A3", addr, size, true, value);
        }
        return true;
    }
    if (zf_primary_enabled && zf_sensorif_primary_address(addr)) {
        if (!z63_z9lab_zf_primary_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("PRIMARY", addr, size, true, value);
        }
        return true;
    }
    if (zf_group_enabled && zf_sensorif_group_address(addr)) {
        if (!z63_z9lab_zf_group_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("GROUP", addr, size, true, value);
        }
        return true;
    }
    if (zf_output_enabled && zf_sensorif_output_address_verified(addr)) {
        if (!z63_z9lab_zf_output_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("OUTPUT", addr, size, true, value);
        }
        return true;
    }
    if (zf_aux_enabled && zf_sensorif_aux_address_verified(addr)) {
        if (!z63_z9lab_zf_aux_write(z63_z9lab, addr, value, size)) {
            zf_block_reject("AUX", addr, size, true, value);
        }
        return true;
    }
    if (!z63_z9lab || !z63_z9lab_range(addr) ||
        !z63_z9lab_write(z63_z9lab, addr, value, size)) {
        return false;
    }
    z63_z9lab_name(addr, true, value);
    return true;
}

/* -machine cfexpress=on: ../z9-qemu's PCIe port-2 adapter (nikon-z9-pcie.h;
 * the attachment section explains the port). */
static const char z63_z9lab_cfexpress_props[] =
    "pcie-dbi-model=on;"
    "pcie-link-diagnostic-mask=4;"
    "pcie-ram-dma-input=on;"
    "pcie-intx-model=on;";

static bool z63_z9lab_init(Object *owner, DeviceState *gic, uint64_t ram_size,
                           MemoryRegion *dram, MemoryRegion *buffer_ext,
                           bool cfexpress, const char *props,
                           const char *raw, Error **errp)
{
    g_autofree char *all = g_strdup_printf("%s;%sstatistics-raw=%s",
                                           props && *props ? props :
                                           z63_z9lab_default_props,
                                           cfexpress ?
                                           z63_z9lab_cfexpress_props : "",
                                           raw ? raw : "");

    if (!raw || !*raw) {
        error_setg(errp, "z9-capture=on needs z9-capture-raw=<NZ9RAW1 file> "
                   "(z63-tools/make_raw.py writes one)");
        return false;
    }
    z63_z9lab = z63_z9lab_new(owner, gic, ram_size, dram, buffer_ext, all,
                              errp);
    if (!z63_z9lab) {
        return false;
    }
    z63_z9lab_pcie = cfexpress;
    z63_z9lab_named = bitmap_new(Z63_Z9LAB_PAGES);
    return true;
}

/* Reuse only the counter implementation. No RAW file, guest RAM fixture,
 * sensor boundary, capture engine, or software event is provided here.
 * The original driver configures/preloads/starts the timer and consumes IRQs.
 */
static bool zf_mio_only_init(Object *owner, DeviceState *gic, uint64_t ram_size,
                             MemoryRegion *dram, MemoryRegion *buffer_ext,
                             bool aux, bool output_config, bool group, bool primary,
                             uint64_t source_period_ns, bool isp1a3, bool isp199, bool source_stop, bool stream_empty, bool isp192, bool isp_small, bool statistics, const char *raw, bool still, bool video_retained, bool video_primary_source, bool video_codec, bool video_host_codec, bool video_primary_exclusive, Error **errp)
{
    const char *props = aux ?
        "bootstrap-fixtures=on;multi-io-counter-model=on;z63-miss-log=on;"
        "event1508-aux-config-model=on;event1508-aux-model=on;event1508-aux1-model=on" :
        "bootstrap-fixtures=on;multi-io-counter-model=on;z63-miss-log=on";
    g_autofree char *group_props = g_strdup_printf("%s%s", props,
        group ? ";event1508-group-model=on" : "");
    g_autofree char *primary_props = g_strdup_printf("%s%s", group_props,
        primary ? ";event1508-channel-model=on;event1508-global0-model=on;"
                  "event1508-global1-model=on;event1508-front-model=on;"
                  "event1508-command-model=on;z63-boundary-ms=0;"
                  "z63-1508-lane0-ack=off;z63-lv-frame-done=off;z63-vd-statistics=off" : "");
    if (source_period_ns && (!primary || !aux)) {
        error_setg(errp, "Zf virtual SIF source requires primary and AUX interfaces");
        return false;
    }
    g_autofree char *source_props = source_period_ns ?
        g_strdup_printf("%s;event1508-timing-model=on;event1508-timing-period-ns=%"
                        PRIu64, primary_props, source_period_ns) : g_strdup(primary_props);
    g_autofree char *isp_props = g_strdup_printf("%s%s", source_props,
        isp1a3 ? ";config150d-model=on" : "");
    if (source_stop && !source_period_ns) {
        error_setg(errp, "Zf empty source stop requires an explicit virtual SIF period");
        return false;
    }
    g_autofree char *stop_props = g_strdup_printf("%s%s", isp_props,
        source_stop ? ";event1508-stop-model=on" : "");
    g_autofree char *stream_props = g_strdup_printf("%s%s", stop_props,
        stream_empty ? ";stream154c-stop-model=on" : "");
    if (statistics && (!isp1a3 || !raw || !*raw)) {
        error_setg(errp, "Zf statistics requires ISP configuration and explicit RAW input");
        return false;
    }
    g_autofree char *statistics_props = statistics ?
        g_strdup_printf("%s;engine150c-control-model=on;engine150c-stop-model=on;"
                        "event1508-global0-paired-boundary=on;"
                        "event1508-programmed-clock-model=on;"
                        "z63-lv-frame-done=on;"
                        "primary19d-config-model=on;primary19bc-config-model=on;primary194-config-model=on;"
                        "primary19d-submit-model=on;primary19bc-submit-model=on;primary194-submit-model=on;"
                        "secondary150d-submit-model=on;primary-parameter-control-model=on;"
                        "config1524-model=on;config1524-control100-input=0;engine1524-control-model=on;"
                        "engine1524-mvd-model=on;engine152-control-model=on;"
                        "image-bank-model=on;engine151-control-model=on;engine1543-control-model=on;"
                        "raw-pipeline-input-pair-model=on;"
                        "raw-pipeline-model=on;raw-pipeline-1514-completion-model=on;engine150c-bit2-model=on;"
                        "engine150d-control-model=on;primary-empty-end-model=on;statistics-model=on;statistics-raw=%s",
                        stream_props, raw) : g_strdup(stream_props);
    if (still && !statistics) {
        error_setg(errp, "zf-still requires zf-statistics and explicit RAW input");
        return false;
    }
    g_autofree char *still_props = still ?
        g_strdup_printf("%s;zf-native-still-source=on;zf-frame-gpio=on;zf-mio-capture=on;primary-d980-config-model=on;primary-d980-stream-model=on;primary-d980-packed14-model=on;primary-d980-single-frame-model=on;primary-d980-source-notify-model=on;primary-d980-prepared-start-model=on;primary-d980-channel-boundary-model=on;primary-d980-live-single-model=on;raw-source-boundary-model=on;primary-capture-direct-stop-model=on;engine1543-capture-control-model=on;engine153-config-model=on;engine153-inplace-identity-model=on;engine151-capture-pixels-model=on;engine151-average-result-model=on;engine151-raw-output-model=on;engine1544-config-model=on;engine1544-neutral-copy-model=on;engine152c-config-model=on;engine152c-request-model=on;engine1540-descriptors-model=on;engine1540-parameters-model=on;engine1540-control-model=on;engine1540-frame-pass-model=on;engine15e2-config-model=on;engine15e2-jpeg-model=on;engine15e2-deferred-input-model=on;engine15e2-limit-disabled-model=on;engine15e2-result474-model=on;engine15e2-preflight-model=on;capture-nv16-pipeline-model=on;engine1542-ram-input-control-model=on;capture-thumbnail-nv16-model=on;primary-stopped-source-reset-model=on;primary-preview-reset-model=on;primary-reset-idle-stop-model=on;primary-reset-queued-start-model=on;primary-reset-parameter-retention-model=on;primary20-empty-reset-model=on;event1508-secondary-timing-model=on;engine1543-video-control-model=on;engine151-video-stream-model=on;engine152c-stream-receiver-model=on;video-receiver-before-supplier-model=on;video-peripheral-control-model=on;video-raw-transport-model=on", statistics_props) : g_strdup(statistics_props);
    if (video_retained && !still) {
        error_setg(errp, "zf-video-retained-start requires zf-still");
        return false;
    }
    g_autofree char *video_props = g_strdup_printf("%s%s", still_props,
        video_retained ? ";engine1543-retained-start-model=on" : "");
    if (video_primary_source && !still) {
        error_setg(errp, "zf-video-primary-source requires zf-still");
        return false;
    }
    g_autofree char *video_sensor_props = g_strdup_printf("%s%s%s", video_props,
        video_primary_source ? ";video-primary-sensor-model=on;raw-pipeline-151c-completion-model=on;video-source-end-model=on" : "",
        video_primary_exclusive ? ";video-secondary-sensor-model=off" : "");
    if (video_codec && !still) {
        error_setg(errp, "zf-video-codec requires zf-still");
        return false;
    }
    g_autofree char *video_codec_props = g_strdup_printf("%s%s", video_sensor_props,
        video_codec ? ";capture-stream-rearm-model=on;capture-frame-pool-model=on;"
                      "video-main-output-model=on;video-preview-ram-model=on;wave5-vpu-model=on;"
                      "yuvcmp-config-model=on;yuvcmp-channels-model=on;"
                      "video-yuv-output-events-model=on;video-forced-stop-model=on;"
                      "video-backend-drain-model=on;backend-program-epoch-model=on;"
                      "video-decoder-staged-reset-model=on;"
                      "raw-pipeline-151c-completion-model=on;video-source-end-model=on;"
                      "video-primary-boundaries-model=on;video-primary-input-boundary-model=on;"
                      "video-header-stop-completion-model=on;video-processing-config-model=on;"
                      "video-nr-reset-model=on;video-initial-reset-model=on" : "");
    if (video_host_codec && !video_codec) {
        error_setg(errp, "zf-video-host-codec requires zf-video-codec");
        return false;
    }
    g_autofree char *video_host_codec_props = g_strdup_printf("%s%s", video_codec_props,
        video_host_codec ? ";wave5-host-codec-diagnostic=on" : "");
    z63_z9lab = z63_z9lab_new(owner, gic, ram_size, dram, buffer_ext, video_host_codec_props, errp);
    if (!z63_z9lab) {
        return false;
    }
    if (!statistics && aux && !z63_z9lab_zf_aux_profile_valid(z63_z9lab, group, primary, source_period_ns)) {
        error_setg(errp, "Zf AUX profile dependency/source validation failed");
        return false;
    }
    zf_mio_only = true;
    zf_statistics_enabled = statistics;
    zf_still_enabled = still;
    zf_video_codec_enabled = video_codec;
    if (statistics) {
        error_report("ZF_STATISTICS: Z9 primary controller, stop IRQ106 and RAW block means enabled");
    }
    zf_aux_enabled = aux;
    if (!statistics && output_config && !z63_z9lab_zf_output_profile_valid(z63_z9lab, group, primary, source_period_ns, source_stop)) {
        error_setg(errp, "Zf output configuration profile has an unintended producer");
        return false;
    }
    zf_output_enabled = output_config;
    if (!statistics && group && !z63_z9lab_zf_group_profile_valid(z63_z9lab, primary, source_period_ns, source_stop)) {
        error_setg(errp, "Zf group profile source/initial-state validation failed");
        return false;
    }
    zf_group_enabled = group;
    if (!statistics && primary && !z63_z9lab_zf_primary_profile_valid(z63_z9lab, source_period_ns, source_stop)) {
        error_setg(errp, "Zf primary profile dependency/source validation failed");
        return false;
    }
    zf_primary_enabled = primary;
    if (source_stop) {
        error_report("ZF_SIF_STOP: original active group0 W1C produces global0 bit18; "
                     "explicit immediate empty status-source drain assumption, no pixels/DMA");
    }
    if (!statistics && isp1a3 && !z63_z9lab_zf_isp1a3_profile_valid(z63_z9lab)) {
        error_setg(errp, "Zf ISP1A3 configuration profile has unintended control/submit/data state");
        return false;
    }
    zf_isp1a3_enabled = isp1a3;
    if (!statistics && isp199 && !z63_z9lab_zf_isp199_profile_valid(z63_z9lab)) {
        error_setg(errp, "Zf ISP199 configuration requires closed control/submit/data profile");
        return false;
    }
    zf_isp199_enabled = isp199;
    if (!statistics && stream_empty && !z63_z9lab_zf_stream154c_profile_valid(z63_z9lab)) {
        error_setg(errp, "Zf STREAM154C requires an initially empty no-transfer owner");
        return false;
    }
    zf_stream154c_enabled = stream_empty;
    if (!statistics && isp192 && !z63_z9lab_zf_isp192_profile_valid(z63_z9lab)) {
        error_setg(errp, "Zf ISP192 configuration requires closed control/submit/data profile");
        return false;
    }
    zf_isp192_enabled = isp192;
    if (!statistics && isp_small && !z63_z9lab_zf_isp_small_profile_valid(z63_z9lab)) {
        error_setg(errp, "Zf ISP small configuration requires closed control/submit/data profile");
        return false;
    }
    zf_isp_small_enabled = isp_small;
    if (isp_small) {
        object_property_add_str(z63_z9lab, "zf-isp-small-config-state",
                                z63_z9lab_zf_isp_small_state, NULL);
        error_report("ZF_ISP_SMALL: complete195/197/198 ordinary words33/44/35 in original "
                     "owners, raw32 RMW197+0; no results/submit/IRQ/DMA");
    }
    if (isp192) {
        object_property_add_str(z63_z9lab, "zf-isp192-config-state",
                                z63_z9lab_zf_isp192_state, NULL);
        error_report("ZF_ISP192: two51-word ordinary instances in original Z9 owner; "
                     "native read10/raw32 RMW; no results/270/submit/IRQ/DMA");
    }
    if (stream_empty) {
        object_property_add_str(z63_z9lab, "zf-stream154c-owner-state",
                                z63_z9lab_zf_stream154c_state, NULL);
        error_report("ZF_STREAM154C: three-channel ordinary config and original empty-only "
                     "query/reset/stop/W1C; no DMA, completed buffer or IRQ producer");
    }
    if (isp199) {
        object_property_add_str(z63_z9lab, "zf-isp199-config-state",
                                z63_z9lab_zf_isp199_state, NULL);
        error_report("ZF_ISP199: two1596-word ordinary instances in original Z9 owner; "
                     "native reads10/14/1704, retained RMW highbits, no result/submit/IRQ/DMA");
    }
    if (isp1a3) {
        object_property_add_str(z63_z9lab, "zf-isp1a3-config-state",
                                z63_z9lab_zf_isp1a3_state, NULL);
        error_report("ZF_ISP1A3:912 own core/lane/LUT words in same Z9 owner; original "
                     "bank counts/secondaryvalid bookkeeping, no submit or data completion");
    }
    if (primary) {
        object_property_add_str(z63_z9lab, "zf-primary-control-state",
                                z63_z9lab_zf_primary_state, NULL);
        error_report("ZF_PRIMARY: shared FRONT config/latch and native control/mask/W1C; "
                     "16bit channel mask, raw32 ACK; virtual period=%" PRIu64
                     ", no RAW/DMA source", source_period_ns);
    }
    if (group) {
        error_report("ZF_GROUP: two bitmap/submit interfaces; explicit idle-read0 "
                     "hypothesis, snapshot/count only, no clear/running/event producer");
    }
    if (output_config) {
        object_property_add_str(z63_z9lab, "zf-output-config-state",
                                z63_z9lab_zf_output_state, NULL);
        error_report("ZF_OUTPUT: own15-word shared configuration shadow, no command/status/event");
    }
    if (source_period_ns) {
        error_report("ZF_SIF_SOURCE: declared virtual period=%" PRIu64
                     "ns; original control[0]/control[3] gates, base AUXbit1/channel0/"
                     "global IRQ model; physical timing unverified, no software event or DMA injection",
                     source_period_ns);
    }
    z63_z9lab_named = bitmap_new(Z63_Z9LAB_PAGES);
    error_report("ZF_MIO_COUNTER: 0x0e000000..0x0e001fff; "
                 "functional 100MHz/(divider+1) clock, physical timing unverified");
    if (aux) {
        error_report("ZF_AUX: own-bound bank0/1 channel0 configuration, pending/W1C/IRQ; "
                     "initial pending0; periodic source only with explicit SIF period");
    }
    return true;
}

#endif /* HW_ARM_NIKON_Z63_Z9LAB_H */
