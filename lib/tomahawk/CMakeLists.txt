#Resolve headers and linkage
add_subdirectory(${TH_DIRECTORY} tomahawk)
link_directories(${TH_DIRECTORY})
target_include_directories(lynx PRIVATE ${TH_DIRECTORY})
target_link_libraries(lynx PRIVATE tomahawk)