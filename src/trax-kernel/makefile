all: riscv-dissasm

./riscv/kernel: main.cpp
	riscv64-unknown-elf-g++ -mno-relax -nostartfiles -emain -Wstack-usage=512 -mabi=lp64f -Ofast -I ../../include main.cpp -o ./riscv/kernel

riscv-dissasm: ./riscv/kernel
	riscv64-unknown-elf-objdump -d -x ./riscv/kernel > ./riscv/kernel.dump

clean:
	rm -f ./riscv/kernel ./riscv/kernel.dump

.PHONY: all clean riscv-dissasm
