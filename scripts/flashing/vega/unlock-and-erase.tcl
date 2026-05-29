# Just issue the volatile WREN + WRSR unlock and erase a sector range.
# Useful when you want to wipe the chip without writing anything.
#
# Required Tcl variables:
#   FIRST_SECTOR  first sector index
#   LAST_SECTOR   last sector index (inclusive). Use 63 for the whole W25Q32.

set HERE [file dirname [info script]]
source [file join $HERE _common.tcl]

if {![info exists ::FIRST_SECTOR]} { error "FIRST_SECTOR not set" }
if {![info exists ::LAST_SECTOR]}  { error "LAST_SECTOR not set" }

lg_open /tmp/flash-progress.log

adapter_khz 8000
halt
lg "UNLOCK status registers"
w25q_unlock
lg "ERASE sectors $::FIRST_SECTOR..$::LAST_SECTOR"
set t0 [clock seconds]
flash erase_sector 0 $::FIRST_SECTOR $::LAST_SECTOR
lg "  done in [expr {[clock seconds] - $t0}] s"
lg_close
shutdown
