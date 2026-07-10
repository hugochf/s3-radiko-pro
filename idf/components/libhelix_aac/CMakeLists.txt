# Helix AAC decoder (RealNetworks RPSL/RCSL). Vendored from pschatzmann/
# arduino-libhelix (src/libhelix-aac). Fixed-point, self-contained C.
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "include"
)

# Third-party code — not -Werror clean and full of intentional casts/shifts.
# Silence warnings so it doesn't fail our stricter build.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
