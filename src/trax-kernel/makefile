make: riscv riscv-dissasm

#-mno-relax 
riscv: ./*
	riscv64-unknown-elf-g++ -mno-relax -nostartfiles -emain -Wstack-usage=512 -mabi=lp64f -Ofast -I ../../include ./main.cpp -o ./riscv/kernel
	#clang++-20 --target=riscv64-unknown-elf --gcc-toolchain=/opt/riscv --sysroot=/opt/riscv/riscv64-unknown-elf -march=rv64imaf -mabi=lp64f -mno-relax -nostartfiles -Wl,-e,main -Wframe-larger-than=512 -O3 -I ../../include ./main.cpp -o ./kernel-llvm

riscv-dissasm: riscv
	riscv64-unknown-elf-objdump -d -x ./riscv/kernel > "./riscv/kernel.dump"
	#riscv64-unknown-elf-objdump -d -x ./kernel-llvm > "./kernel-llvm.dump"

clean:
	rm -f riscv/*