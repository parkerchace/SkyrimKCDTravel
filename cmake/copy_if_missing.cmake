# Copies SRC to DST only when DST does not already exist, so a user's edited
# config is never overwritten by a rebuild.
if(NOT EXISTS "${DST}")
    configure_file("${SRC}" "${DST}" COPYONLY)
endif()
