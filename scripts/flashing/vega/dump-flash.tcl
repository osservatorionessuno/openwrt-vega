# Dump the SPI NOR flash via XIP to a file.
# Required Tcl variables:
#   OUT_PATH   destination binary
#   DUMP_SIZE  bytes to read (decimal or 0x... hex). Default 0x400000 (4 MB)

if {![info exists ::OUT_PATH]}  { error "OUT_PATH not set" }
if {![info exists ::DUMP_SIZE]} { set ::DUMP_SIZE 0x400000 }

adapter_khz 8000
halt
dump_image $::OUT_PATH 0x20000000 $::DUMP_SIZE
shutdown
