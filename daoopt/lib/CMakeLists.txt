# Zlib library
add_subdirectory(zlib)

# SLS4MPE code by Frank Hutter
add_subdirectory(sls4mpe)

# GzStream library (needs gzip)
add_library(Gzstream OBJECT gzstream.cpp)
