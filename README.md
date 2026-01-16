# Lebirun2

This is a small project that i made since im really interested in doing OSDev. This is the successor to Lebirun C (Lebirun but made in C, rather than being made in a C# Library called Cosmos). This is just a different take on it. Note that this is still called Lebirun in the code, and should generally NOT be changed to Lebirun2.

### How to Compile
First of all, you need i686-elf-gcc (feel free to get it either pre-built from somewhere OR built by yourself, it doesn't matter that much i guess).
If you have i686-elf-gcc now, just execute `./build.sh` (or `./qemu.sh` if you want for Lebirun to build AND QEMU to launch).

### How to Configure
Just run "./lebconfig.sh", and then just do things there. After that, just save the file.

#### Advanced Configuration
- Press **G** in lebconfig to access Settings
- **Current default config**: Set which config file lebconfig loads by default
- **Auto-set default config at save**: When enabled, saving to a custom filename (e.g., `.happyconf`) automatically:
  1. Sets it as your default config for future lebconfig launches
  2. Writes the config to `.config` so the kernel/OS uses it for builds
  3. Saves your preferences to `.lebconfig_settings`

This allows you to maintain multiple config files while ensuring the kernel always uses your preferred one.