# Codec2 1.2.0 (David Rowe and others, LGPL-2.1; see COPYING and THIRD_PARTY.md), the speech codec
# behind voice over LoRa (D72, D78, D95). Only what the 1200 bit/s mode needs is here: the vocoder, its
# FFT, and the codebooks, which tools/gen_codec2_codebooks.py makes from the text tables in codebook/.
# The modem, FreeDV and file tools are left out. Nothing in src/ is modified, so a newer Codec2 can
# be dropped in; the settings below are how it is built for a microcontroller.
idf_component_register(SRCS "lg_codec2_port.c" "src/codec2.c" "src/codec2_fft.c" "src/interp.c" "src/kiss_fft.c" "src/kiss_fftr.c"
                            "src/lpc.c" "src/lsp.c" "src/mbest.c" "src/nlp.c" "src/pack.c" "src/phase.c"
                            "src/postfilter.c" "src/quantise.c" "src/sine.c"
                            "src/codebook.c" "src/codebookd.c" "src/codebookjmv.c" "src/codebookge.c"
                       INCLUDE_DIRS "include" "src")
# One mode: every other one compiles to nothing and its tables are dropped at link time.
target_compile_definitions(${COMPONENT_LIB} PRIVATE __EMBEDDED__=1 CODEC2_MODE_EN_DEFAULT=0 CODEC2_MODE_1200_EN=1)
target_compile_definitions(${COMPONENT_LIB} INTERFACE CODEC2_MODE_EN_DEFAULT=0 CODEC2_MODE_1200_EN=1)
# Third-party code is built as its authors wrote it: its warnings are not ours to fix.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -fsingle-precision-constant)
