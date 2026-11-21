.section .rodata.module,"a"
.balign 8
.global real_module
.global real_module_end
real_module:
.incbin "/kmod/out/earc_route-4.4.ko"
real_module_end:
