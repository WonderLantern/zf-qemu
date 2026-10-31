/* Unit harness declaration; production uses QEMU's real entropy provider. */
void qemu_guest_getrandom_nofail(void *buffer, size_t length);
