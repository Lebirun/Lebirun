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

### Cloning

Whenever you clone this repository, please add --recurse-submodules to the git clone command. Otherwise, the operating system will refuse to build.

### License

This project is licensed under the GNU General Public License v3. 

### Donate

If you would like to support this project, please consider making a donation.

- **BTC:** `bc1qz0aaxg4a4h40r6xq2c597v5zt6lq8z9fv58yjw`
- **ETH:** `0x29ad4604b4fc39037d973f65ffb34cbda65916c1`
- **SOL:** `5HKmrrJg65jjFum8a8iJDkcwS1mwrfzet5MwN8pGks4B`