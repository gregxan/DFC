# Rev 2.25: writes build_id.c with the build's date and time. Run on every
# build (see CMakeLists.txt), so the report and the updater's HELLO always
# identify the image that was just linked.
string(TIMESTAMP ts "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}.tmp" "#include \"build_id.h\"\nconst char g_build_id[] = \"${ts}\";\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
