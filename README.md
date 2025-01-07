# Little Computer 3 Virtual Machine

This is a basic implementation of a Little Computer 3 (lc3) CPU architecture VM.

Created by following [Justin Meiners's](https://www.jmeiners.com/) and [Ryan Pendleton's](https://www.ryanp.me/)
article ["Write your Own Virtual Machine"](https://www.jmeiners.com/lc3-vm/).


## Running

VM:
```bash
gcc -o lc3 lc3.c
./lc3 examples/hello_world.obj
```

lc3tools:
```bash
cd lc3tools
./configure
make dist_lc3as
```

## TODO

- [ ] examples/2048 doesn't run (input is broken?)
- [ ] examples/Rogue doesn't run (input is broken?)
- [ ] More example programs
- [ ] Debugging, i.e. memory and registers
- [ ] Fix building of lc3tools without any modifications

