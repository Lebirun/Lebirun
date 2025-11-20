# Made to help myself remember the commands for debugging
`layout regs` →  see registers live
`stepi / ni` →  single-step
`p/x $cr3` →  print CR3
`p/x *(uint32_t*)0xFFFFF000` →  read PDE[0] via recursive mapping
`info registers`
`x/10i $eip` →  disassemble around PC
