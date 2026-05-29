# Shared NUSPI helpers and W25Q32 status-register unlock.
# Sourced by other flash-*.tcl scripts via `source`.

set NUSPI  0x10014000
set FCTRL  [expr {$NUSPI + 0x60}]
set CSMODE [expr {$NUSPI + 0x18}]
set FMT    [expr {$NUSPI + 0x40}]
set TXDATA [expr {$NUSPI + 0x48}]
set TXMARK [expr {$NUSPI + 0x50}]
set IP     [expr {$NUSPI + 0x74}]

proc rd_word {a} { lindex [read_memory $a 32 1] 0 }

proc spi_set_dir {d} {
    global FMT
    set f [rd_word $FMT]
    mww $FMT [expr {($f & ~0x8) | (($d & 1) << 3)}]
}

proc spi_tx {b} {
    global TXDATA
    for {set i 0} {$i < 1000} {incr i} {
        if {!([rd_word $TXDATA] & 0x80000000)} break
    }
    mww $TXDATA [expr {$b & 0xff}]
}

proc spi_wait_txwm {} {
    global IP
    for {set i 0} {$i < 1000} {incr i} {
        if {[rd_word $IP] & 1} return
    }
}

# Issue a single CS-asserted SPI transaction with the given bytes.
# Used for status-register unlock (WREN + WRSR + WRSR2).
proc spi_cmd {bytes} {
    global FCTRL CSMODE TXMARK
    set fc [rd_word $FCTRL]
    mww $FCTRL [expr {$fc & ~1}]
    mww $TXMARK 1
    spi_wait_txwm
    spi_set_dir 1
    mww $CSMODE 2
    foreach b $bytes { spi_tx $b }
    spi_wait_txwm
    mww $CSMODE 0
    spi_set_dir 1
    mww $FCTRL [expr {$fc | 1}]
}

# Volatile unlock — clears BP/CMP/WPS/SRP. Idempotent.
proc w25q_unlock {} {
    spi_cmd {0x50}
    spi_cmd {0x01 0x00 0x00}
    spi_cmd {0x50}
    spi_cmd {0x31 0x00}
}

# Logging helper writing to both a file (line-buffered) and stdout.
proc lg_open {path} {
    set ::LOG [open $path w]
    fconfigure $::LOG -buffering line
}
proc lg {msg} {
    set line "[clock format [clock seconds] -format %T] $msg"
    if {[info exists ::LOG]} { puts $::LOG $line; flush $::LOG }
    catch {puts stdout $line; flush stdout}
}
proc lg_close {} {
    if {[info exists ::LOG]} { close $::LOG; unset ::LOG }
}
