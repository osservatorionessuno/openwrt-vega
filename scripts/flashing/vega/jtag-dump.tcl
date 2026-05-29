# Halt the CPU and print a compact debug snapshot.
# Used by jtag-dump.sh.
adapter_khz 8000
halt

puts "===== ARCHITECTURAL ====="
foreach r {pc ra sp gp tp t0 t1 t2 fp a0 a1 a2 a3 a4 a5} {
    puts "$r       = [reg $r]"
}

puts ""
puts "===== PRIVILEGE / MMU ====="
foreach r {priv satp mstatus sstatus dcsr} {
    catch { puts "$r       = [reg $r]" }
}

puts ""
puts "===== M-MODE TRAP ====="
foreach r {mcause mepc mtval mtvec mie mip} {
    catch { puts "$r       = [reg $r]" }
}

puts ""
puts "===== S-MODE TRAP ====="
foreach r {scause sepc stval stvec sie sip} {
    catch { puts "$r       = [reg $r]" }
}

puts ""
puts "===== CPU ID ====="
foreach r {misa mvendorid marchid mimpid mhartid} {
    catch { puts "$r       = [reg $r]" }
}

puts ""
puts "===== BYTES @ 0x412000a0..0x412000df ====="
set v [read_memory 0x412000a0 8 64]
set line ""; set i 0
foreach b $v {
    if {$i == 0} { set line "412000a0: " }
    append line [format "%02x " [expr {$b & 0xff}]]
    incr i
    if {($i % 16) == 0} { puts $line; set line "" }
}

puts ""
puts "===== uImage Image-header @ 0x41200000 (64 bytes) ====="
set v [read_memory 0x41200000 8 64]
set line ""; set i 0
foreach b $v {
    if {$i == 0} { set line "41200000: " }
    append line [format "%02x " [expr {$b & 0xff}]]
    incr i
    if {($i % 16) == 0} { puts $line; set line "" }
}

shutdown
