# Erase + write + verify a contiguous sector range of the NOR flash.
#
# Required Tcl variables (set via openocd -c "set NAME VALUE"):
#   IMG_PATH      absolute path to the binary to flash
#   FIRST_SECTOR  first sector index (W25Q32: 0..63, 64 KB each)
#   LAST_SECTOR   last sector index (inclusive)
#   LOG_PATH      where to write the progress log (default /tmp/flash-progress.log)
#
# The file must cover the *whole* flash (offsets in-file match offsets in-chip).

set HERE [file dirname [info script]]
source [file join $HERE _common.tcl]

if {![info exists ::IMG_PATH]}     { error "IMG_PATH not set" }
if {![info exists ::FIRST_SECTOR]} { error "FIRST_SECTOR not set" }
if {![info exists ::LAST_SECTOR]}  { error "LAST_SECTOR not set" }
if {![info exists ::LOG_PATH]}     { set ::LOG_PATH /tmp/flash-progress.log }

lg_open $::LOG_PATH

set SECTOR_SIZE 65536
set first  $::FIRST_SECTOR
set last   $::LAST_SECTOR
set nsect  [expr {$last - $first + 1}]
set bytes  [expr {$nsect * $SECTOR_SIZE}]
set addr   [expr {0x20000000 + $first * $SECTOR_SIZE}]

adapter_khz 8000
riscv autofence off
halt

lg "UNLOCK status registers"
w25q_unlock

lg "ERASE sectors $first..$last ($nsect x 64 KB = [expr {$bytes/1024}] KB)"
set t0 [clock seconds]
flash erase_sector 0 $first $last
lg "  erase done in [expr {[clock seconds] - $t0}] s"

lg "WRITE [expr {$bytes/1024}] KB @ [format 0x%08x $addr] from $::IMG_PATH"
set t0 [clock seconds]
flash write_bank 0 $::IMG_PATH [expr {$first * $SECTOR_SIZE}]
set dt [expr {[clock seconds] - $t0}]
if {$dt == 0} { set dt 1 }
lg "  write done in ${dt} s ([format %.2f [expr {double($bytes)/1024.0/$dt}]] KB/s)"

# Spot-verify: read first 16 bytes of each sector, compare to the file.
set fh [open $::IMG_PATH rb]
fconfigure $fh -translation binary
set all_ok 1
for {set i 0} {$i < $nsect} {incr i} {
    set s_addr [expr {$addr + $i * $SECTOR_SIZE}]
    seek $fh [expr {($first + $i) * $SECTOR_SIZE}]
    set raw [read $fh 16]
    binary scan $raw iuiuiuiu w0 w1 w2 w3
    set v [read_memory $s_addr 32 4]
    set ok 1
    foreach exp [list $w0 $w1 $w2 $w3] got $v {
        if {$exp != $got} { set ok 0; break }
    }
    set sect_idx [expr {$first + $i}]
    if {$ok} {
        lg "  sector $sect_idx @[format 0x%08x $s_addr] VERIFY OK"
    } else {
        lg "  sector $sect_idx @[format 0x%08x $s_addr] FAIL exp=[list [format 0x%08x $w0] [format 0x%08x $w1] [format 0x%08x $w2] [format 0x%08x $w3]] got=$v"
        set all_ok 0
    }
}
close $fh
if {$all_ok} { lg "ALL $nsect sectors OK" } else { lg "VERIFY FAILED" }
lg_close
shutdown
