<p align="center">
  <img src="./images/Lebirun-full.png" alt="The full Lebirun2 logo" width="250" />
</p>

# Lebirun
A hobby operating system that is the successor to Lebirun C, which is a successor to the original Lebirun which is made in C# using the Cosmos library.

### Build
On Debian or Debian-based, install the required host tools with:

```sh
sudo apt install build-essential grub-common grub-pc-bin xorriso squashfs-tools
```

To compile Lebirun, you will also need x86_64-elf-gcc (a cross-compiler for x86_64). To start building the kernel, type in `./build.sh`. If you need to launch QEMU (either before or after building), type in `./qemu.sh`.

### Configuration
The configuration of Lebirun is powered by Lebconfig, an alternative to Linux's Menuconfig made from the ground up. In order to enter the configuration user interface, simply type in "./lebconfig.sh" while being in the cloned Lebirun directory.

Lebconfig will need ncurses. Install it on Debian/Debian-based with:
```sh
sudo apt install libncurses-dev
```

### License

This project is licensed under the GNU General Public License v3. 

Effective 9 July 2026, this project has officially transitioned from GPLv2 to GPLv3. We (The Lebirun Project), as the copyright holders of the code within this repository's history, hereby license all historical commits, modifications, and past releases under the GNU General Public License v3.0 or later.
