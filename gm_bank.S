/*
 * Embeds the packed GM.DLS wavetable bank into flash (.rodata, XIP-resident).
 *
 * Generate the bank first (offline, on the host):
 *     dls_pack gm.dls gm_bank.bin <SOUND_FREQUENCY>      // e.g. 22050
 *
 * Add this file to the firmware target's sources, and make gm_bank.bin findable
 * by the assembler (incbin path is resolved relative to the assembler include
 * dirs). In CMake, e.g.:
 *     target_sources(<fw> PRIVATE emulator/audio/gm_bank.S)
 *     set_property(SOURCE emulator/audio/gm_bank.S APPEND PROPERTY
 *                  COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/emulator/audio")
 * (or point the -I at wherever gm_bank.bin is generated).
 *
 * NOTE: .rodata keeps the 3 MB PCM in flash on purpose — do NOT mark it
 * __not_in_flash; it must not land in the 264 KB SRAM.
 */
    .section .rodata
    .p2align 2
    .global gm_bank_blob
gm_bank_blob:
    .incbin "gm_bank.bin"
    .p2align 2
    .global gm_bank_blob_end
gm_bank_blob_end:
