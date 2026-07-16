# Contributing
If you want to contribute to this project, you have to fulfill these:
- Do not add extra comments to the code (comments kind of clutter the code belonging to the amount of comments added, and i tend to remove comments), having a comment at the top of files that you want to license differently and small infos are okay though. If you are contributing code with an LLM, tell it to not include any comments for now.
- Always test your code before sending your patches, so you won't need to do more changes after sending the pull request.
- Attempt to **not** increase memory usage much.

Tips:
- If you are willing to contribute with the usage of LLM, consider using the latest and best models instead of something quite old or weak.

Development Setup
- x86_64-elf-gcc for building the OS, libc and anything in userprog
- Literally any text editor, IDE or whatever