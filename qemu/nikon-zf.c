// SPDX-License-Identifier: GPL-2.0-or-later
/* Zf C3.01: reuse the EXPEED7 devices; keep board identity explicit.
 * No firmware, keys, captured application state or firmware patches here.
 */
#include "qemu/osdep.h"
#include "hw/boards.h"
#include "qapi/error.h"
#include "qom/object.h"

static void nikon_zf_instance_init(Object *obj)
{
    /* The Zf initial page mappings are identity mappings. Its own later
     * initialization copies/remaps text; Z63's preload is not its reset.
     * The Z63 synthetic MCU has model-specific replies and is not a Zf MCU.
     */
    object_property_set_bool(obj, "image-in-dram", false, &error_abort);
    object_property_set_bool(obj, "mcu-peer", false, &error_abort);
    /* Own EX McuRxSvc arms SPI1 DMA then raises PB14; own EG GPIO 0x21
     * reads port 3 bit 2. boot34/35 isolates the inherited PB12 mismatch:
     * PB14 starts native frames, checksums and D12 ACK. Own ShterSvc's
     * descriptor at 20019410+18 is PG8 (request EXTI); +14 is PD7
     * (completion). PF4 is a matrix row and must not drive this request.
     */
    object_property_set_str(obj, "mcu-wires",
        "B14=a3:2+E3=a3:3+D12=a4:3+G6=a4:7+G8=a3:12+D7=a3:7", &error_abort);
}

static void nikon_zf_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Nikon Zf C3.01 EXPEED7 research machine (local bring-up)";
}

static const TypeInfo nikon_zf_type = {
    .name = MACHINE_TYPE_NAME("nikon-zf"),
    .parent = MACHINE_TYPE_NAME("nikon-z63"),
    .instance_init = nikon_zf_instance_init,
    .class_init = nikon_zf_class_init,
};

static void nikon_zf_register_types(void)
{
    type_register_static(&nikon_zf_type);
}
type_init(nikon_zf_register_types)
