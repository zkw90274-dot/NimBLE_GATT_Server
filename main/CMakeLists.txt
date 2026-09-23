file(GLOB_RECURSE srcs "main.c" "src/*.c")

# Vendored Fusion AHRS. Kept in its own directory with upstream file names and
# contents untouched so it can be diffed against a newer release - which also
# means its headers must sit next to its sources (upstream uses same-directory
# #include), hence the extra INCLUDE_DIRS entry instead of splitting them into
# include/. See fusion/README.md.
file(GLOB_RECURSE fusion_srcs "fusion/*.c")

idf_component_register(SRCS "${srcs}" ${fusion_srcs}
                       INCLUDE_DIRS "./include" "./fusion")
